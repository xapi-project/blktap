/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include "io-optimize.h"
#include "profile.h"

#if (!defined(TEST) && IO_TRACE == 1)
#define DBG(ctx, f, a...) BLOG((ctx)->ihandle, f, ##a)
#elif defined(TEST)
#define DBG(ctx, f, a...) printf(f, ##a)
#else
#define DBG(ctx, f, a...) ((void)0)
#endif

static void print_merged_iocbs(struct opioctx *ctx, 
			       struct iocb **iocbs, int num_iocbs);

void
opio_free(struct opioctx *ctx)
{
	free(ctx->opios);
	free(ctx->free_opios);
	free(ctx->iocb_queue);
	free(ctx->event_queue);
}

int
opio_init(struct opioctx *ctx, int num_iocbs)
{
	int i;

	memset(ctx, 0, sizeof(struct opioctx));

	ctx->num_opios     = num_iocbs;
	ctx->free_opio_cnt = num_iocbs;
	ctx->opios         = calloc(1, sizeof(struct opio) * num_iocbs);
	ctx->free_opios    = calloc(1, sizeof(struct opio *) * num_iocbs);
	ctx->iocb_queue    = calloc(1, sizeof(struct iocb *) * num_iocbs);
	ctx->event_queue   = calloc(1, sizeof(struct io_event) * num_iocbs);

	if (!ctx->opios || !ctx->free_opios ||
	    !ctx->iocb_queue || !ctx->event_queue)
		goto fail;

	for (i = 0; i < num_iocbs; i++)
		ctx->free_opios[i] = &ctx->opios[i];

	return 0;

 fail:
	opio_free(ctx);
	return -ENOMEM;
}

static inline struct opio *
alloc_opio(struct opioctx *ctx)
{
	if (ctx->free_opio_cnt <= 0)
		return NULL;
	return ctx->free_opios[--ctx->free_opio_cnt];
}

static inline void
free_opio(struct opioctx *ctx, struct opio *op)
{
	memset(op, 0, sizeof(struct opio));
	ctx->free_opios[ctx->free_opio_cnt++] = op;
}

static inline void
restore_iocb(struct opio *op)
{
	struct iocb *io = op->iocb;

	io->data        = op->data;
	io->u.c.buf     = op->buf;
	io->u.c.nbytes  = op->nbytes;
}

static inline int
iocb_optimized(struct opioctx *ctx, struct iocb *io)
{
	unsigned long iop   = (unsigned long)io->data;
	unsigned long start = (unsigned long)ctx->opios;
	unsigned long end   = start + (ctx->num_opios * sizeof(struct opio));

	return (iop >= start && iop < end);
}

static inline int
contiguous_sectors(struct iocb *l, struct iocb *r)
{
	return (l->u.c.offset + l->u.c.nbytes == r->u.c.offset);
}

static inline int
contiguous_buffers(struct iocb *l, struct iocb *r)
{
	return (l->u.c.buf + l->u.c.nbytes == r->u.c.buf);
}

static inline void
init_opio_list(struct opio *op)
{
	op->list.head = op->list.tail = op;
}

static struct opio *
opio_iocb_init(struct opioctx *ctx, struct iocb *io)
{
	struct opio *op;

	op = alloc_opio(ctx);
	if (!op)
		return NULL;

	op->buf    = io->u.c.buf;
	op->nbytes = io->u.c.nbytes;
	op->offset = io->u.c.offset;
	op->data   = io->data;
	op->iocb   = io;
	io->data   = op;

	init_opio_list(op);

	return op;
}

static inline struct opio *
opio_get(struct opioctx *ctx, struct iocb *io)
{
	if (iocb_optimized(ctx, io))
		return (struct opio *)io->data;
	else
	        return opio_iocb_init(ctx, io);
}

static int
merge_tail(struct opioctx *ctx, struct iocb *head, struct iocb *io)
{
	struct opio *ophead, *opio;

	ophead = opio_get(ctx, head);
	if (!ophead)
		return -ENOMEM;

	opio = opio_get(ctx, io);
	if (!opio)
		return -ENOMEM;

	opio->head        = ophead;
	head->u.c.nbytes += io->u.c.nbytes;
	ophead->list.tail = ophead->list.tail->next = opio;
	
	return 0;
}

static int
merge(struct opioctx *ctx, struct iocb *head, struct iocb *io)
{
	if (head->aio_lio_opcode != io->aio_lio_opcode)
		return -EINVAL;

	if (!contiguous_sectors(head, io) || !contiguous_buffers(head, io))
		return -EINVAL;

	return merge_tail(ctx, head, io);		
}

int
io_merge(struct opioctx *ctx, struct iocb **queue, int num)
{
	int i, on_queue;
	struct iocb *io, **q;
	struct opio *ophead;
	
	if (!num)
		return 0;

	on_queue = 0;
	q = ctx->iocb_queue;
	memcpy(q, queue, num * sizeof(struct iocb *));

	for (i = 1; i < num; i++) {
		io = q[i];
		if (merge(ctx, queue[on_queue], io) != 0)
			queue[++on_queue] = io;
	}

#if (defined(TEST) || IO_TRACE == 1)
	print_merged_iocbs(ctx, queue, on_queue + 1);
#endif

	return ++on_queue;
}

static int
expand_iocb(struct opioctx *ctx, struct iocb **queue, struct iocb *io)
{
	int idx;
	struct opio *op, *next;

	idx = 0;
	op  = (struct opio *)io->data;
	while (op) {
		next = op->next;
		restore_iocb(op);
		queue[idx++] = op->iocb;
		free_opio(ctx, op);
		op   = next;
	}

	return idx;
}

int
io_expand_iocbs(struct opioctx *ctx, struct iocb **queue, int idx, int num)
{
	int i, on_queue;
	struct iocb *io, **q;

	if (!num)
		return 0;

	on_queue = 0;
	q = ctx->iocb_queue;
	memcpy(q, queue, num * sizeof(struct iocb *));

	for (i = idx; i < num; i++) {
		io = q[i];
		if (!iocb_optimized(ctx, io))
			queue[on_queue++] = io;
		else
			on_queue += expand_iocb(ctx, queue + on_queue, io);
	}

	return on_queue;
}

static int
expand_event(struct opioctx *ctx,
	     struct io_event *event, struct io_event *queue, int idx)
{
	int err;
	struct iocb *io;
	struct io_event *ep;
	struct opio *ophead, *op, *next;

	io     = event->obj;
	ophead = (struct opio *)io->data;
	op     = ophead;
	err    = (event->res == io->u.c.nbytes ? 0 : -EIO);

	while (op) {
		next    = op->next;
		ep      = &queue[idx++];
		ep->obj = op->iocb;
		ep->res = (err ? err : op->nbytes);
		restore_iocb(op);
		free_opio(ctx, op);
		op      = next;
	}

	return idx;
}

int
io_split(struct opioctx *ctx, struct io_event *events, int num)
{
	int on_queue;
	struct iocb *io;
	struct io_event *ep, *q;
	
	if (!num)
		return 0;

	on_queue = 0;
	q = ctx->event_queue;
	memcpy(q, events, num * sizeof(struct io_event));

	for (ep = q; num-- > 0; ep++) {
		io = ep->obj;
		if (!iocb_optimized(ctx, io))
			events[on_queue++] = *ep;
		else
			on_queue = expand_event(ctx, ep, events, on_queue);
	}

	return on_queue;
}

void
io_debug(struct opioctx *ctx)
{
#if (IO_TRACE == 1)
	BDUMP("/tmp/io-optimize.log", ctx->ihandle);
#endif
}

/******************************************************************************
debug print functions
******************************************************************************/
static inline void
__print_iocb(struct opioctx *ctx, struct iocb *io, char *prefix)
{
	char *type;

	type = (io->aio_lio_opcode == IO_CMD_PREAD ? "read" : "write");

	DBG(ctx, "%soff: %08llx, nbytes: %04lx, buf: %p, type: %s, data: %08lx,"
	    " optimized: %d\n", prefix, io->u.c.offset, io->u.c.nbytes, 
	    io->u.c.buf, type, (unsigned long)io->data, 
	    iocb_optimized(ctx, io));
}

static char *null_prefix = "";
#define print_iocb(ctx, io) __print_iocb(ctx, io, null_prefix)

static void
print_iocbs(struct opioctx *ctx, struct iocb **iocbs, int num_iocbs)
{
	int i;
	char pref[10];
	struct iocb *io;

	DBG(ctx, "iocbs:\n");
	for (i = 0; i < num_iocbs; i++) {
		io = iocbs[i];
		snprintf(pref, 10, "%d: ", i);
		__print_iocb(ctx, io, pref);
	}
}

static void
print_optimized_iocbs(struct opioctx *ctx, struct opio *op, int *cnt)
{
	char pref[10];

	while (op) {
		snprintf(pref, 10, "  %d: ", (*cnt)++);
		__print_iocb(ctx, op->iocb, pref);
		op = op->next;
	}
}

static void
print_merged_iocbs(struct opioctx *ctx, struct iocb **iocbs, int num_iocbs)
{
	int i, cnt;
	char pref[10];
	struct iocb *io;
	struct opio *op;

	DBG(ctx, "merged iocbs:\n");
	for (i = 0, cnt = 0; i < num_iocbs; i++) {
		io = iocbs[i];
		snprintf(pref, 10, "%d: ", cnt++);
		__print_iocb(ctx, io, pref);

		if (iocb_optimized(ctx, io)) {
			op = (struct opio *)io->data;
			print_optimized_iocbs(ctx, op->next, &cnt);
		}
	}
}

static void
print_events(struct opioctx *ctx, struct io_event *events, int num_events)
{
	int i;
	struct iocb *io;

	for (i = 0; i < num_events; i++) {
		io = events[i].obj;
		print_iocb(ctx, io);
	}
}
/******************************************************************************
end debug print functions
******************************************************************************/

#if defined(TEST)

#define hmask 0x80000000UL
#define smask 0x40000000UL
#define make_data(idx, is_head, sparse) \
         (void *)((idx) | ((is_head) ? hmask : 0) | ((sparse) ? smask : 0))
#define data_idx(data)          (int)((unsigned long)(data) & (0x0fffffff))
#define data_is_head(data)      (((unsigned long)(data) & hmask) ? 1 : 0)
#define data_is_sparse(data)    (((unsigned long)(data) & smask) ? 1 : 0)

static void
usage(void)
{
	fprintf(stderr, "usage: io_optimize [-n num_runs] "
		"[-i num_iocbs] [-s num_secs] [-r random_seed]\n");
	exit(-1);
}

static int xalloc_cnt, xfree_cnt;
static inline char *
xalloc(int size)
{
	char *buf = malloc(size);
	if (!buf) {
		fprintf(stderr, "xalloc failed\n");
		exit(ENOMEM);
	}
	xalloc_cnt++;
	return buf;
}

static inline void
xfree(void *buf)
{
	free(buf);
	xfree_cnt++;
}

static void
randomize_iocbs(struct iocb **iocbs, int num_iocbs, int num_secs)
{
	int i, j;

	i = 0;
	while (i < num_iocbs) {
		char *buf;
		short type;
		int segs, sparse_mem;
		uint64_t offset, nbytes;
		
		type   = (random() % 10 < 5 ? IO_CMD_PREAD : IO_CMD_PWRITE);
		offset = ((random() % num_secs) << 9);

		if (random() % 10 < 4) {
			segs   = 1;
			nbytes = (((random() % 7) + 1) << 9);
		} else {
			segs   = (random() % 10) + 1;
			nbytes = 4096;
		}

		if (i + segs > num_iocbs)
			segs = (num_iocbs - i);

		sparse_mem = (random() % 10 < 2 ? 1 : 0);

		if (sparse_mem)
			buf = xalloc(nbytes);
		else
			buf = xalloc(segs * nbytes);

		for (j = 0; j < segs; j++) {
			struct iocb *io    = iocbs[i + j];
			io->aio_lio_opcode = type;
			io->u.c.nbytes     = nbytes;
			io->u.c.offset     = offset;
			io->u.c.buf        = buf;
			offset            += nbytes;

			io->data = make_data(i + j, (j == 0), sparse_mem);

			if (j + 1 < segs && sparse_mem)
				buf  = xalloc(nbytes);
			else
				buf += nbytes;
		}

		i += segs;
	}
}

static int
simulate_io(struct iocb **iocbs, struct io_event *events, int num_iocbs)
{
	int i, done;
	struct iocb *io;
	struct io_event *ep;

	if (num_iocbs > 1)
		done = (random() % (num_iocbs - 1)) + 1;
	else
		done = num_iocbs;

	for (i = 0; i < done; i++) {
		io      = iocbs[i];
		ep      = &events[i];
		ep->obj = io;
		ep->res = (random() % 10 < 8 ? io->u.c.nbytes : 0);
	}

	return done;
}

static inline void
process_events(struct opioctx *ctx, 
	       struct iocb *iocb_list, struct io_event *events, int num)
{
	int i;
	struct iocb *io;

	for (i = 0; i < num; i++) {
		io = events[i].obj;
		print_iocb(ctx, io);
		if (data_idx(io->data) != (io - iocb_list)) {
			printf("corrupt data! data_idx = %d, io = %d\n",
			       data_idx(io->data), (io - iocb_list));
			exit(-1);
		}
		if (data_is_head(io->data) || data_is_sparse(io->data))
			xfree(io->u.c.buf);
		memset(io, 0, sizeof(struct iocb));
	}
}

static inline void
init_optest(struct iocb *iocb_list, 
	    struct iocb **iocbs, struct io_event *events, int num)
{
	int i;

	memset(iocb_list, 0, num * sizeof(struct iocb));
	memset(events, 0, num * sizeof(struct io_event));

	for (i = 0; i < num; i++)
		iocbs[i]  = &iocb_list[i];
}

int
main(int argc, char **argv)
{
	uint64_t num_secs;
	struct opioctx ctx;
	struct io_event *events;
	int i, c, num_runs, num_iocbs, seed;
	struct iocb *iocb_list, **iocbs, **ioqueue;

	num_runs  = 1;
	num_iocbs = 300;
	seed      = time(NULL);
	num_secs  = ((4ULL << 20) >> 9); /* 4GB disk */

	while ((c = getopt(argc, argv, "n:i:s:r:h")) != -1) {
		switch (c) {
		case 'n':
			num_runs  = atoi(optarg);
			break;
		case 'i':
			num_iocbs = atoi(optarg);
			break;
		case 's':
			num_secs  = strtoull(optarg, NULL, 10);
			break;
		case 'r':
			seed      = atoi(optarg);
			break;
		case 'h':
			usage();
		case '?':
			fprintf(stderr, "Unrecognized option: -%c\n", optopt);
			usage();
		}
	}

	printf("Running %d tests with %d iocbs on %llu sectors, seed = %d\n",
	       num_runs, num_iocbs, num_secs, seed);

	srand(seed);

	iocb_list = malloc(num_iocbs * sizeof(struct iocb));
	iocbs     = malloc(num_iocbs * sizeof(struct iocb *));
	events    = malloc(num_iocbs * sizeof(struct io_event));
	
	if (!iocb_list || !iocbs || !events || opio_init(&ctx, num_iocbs)) {
		fprintf(stderr, "initialization failed\n");
		exit(ENOMEM);
	}

	for (i = 0; i < num_runs; i++) {
		int op_rem, op_done, num_split, num_events, num_done;

		ioqueue = iocbs;
		init_optest(iocb_list, ioqueue, events, num_iocbs);
		randomize_iocbs(ioqueue, num_iocbs, num_secs);
		print_iocbs(&ctx, ioqueue, num_iocbs);

		op_done  = 0;
		num_done = 0;
		op_rem   = io_merge(&ctx, ioqueue, num_iocbs);
		print_iocbs(&ctx, ioqueue, op_rem);
		print_merged_iocbs(&ctx, ioqueue, op_rem);
		
		while (num_done < num_iocbs) {
			DBG(&ctx, "optimized remaining: %d\n", op_rem);

			DBG(&ctx, "simulating\n");
			num_events = simulate_io(ioqueue + op_done, events, op_rem);
			print_events(&ctx, events, num_events);

			DBG(&ctx, "splitting %d\n", num_events);
			num_split = io_split(&ctx, events, num_events);
			print_events(&ctx, events, num_split);

			DBG(&ctx, "processing %d\n", num_split);
			process_events(&ctx, iocb_list, events, num_split);

			op_rem   -= num_events;
			op_done  += num_events;
			num_done += num_split;
		}

		DBG(&ctx, "run %d: processed: %d, xallocs: %d, xfrees: %d\n", 
		    i, num_done, xalloc_cnt, xfree_cnt);
		if (xalloc_cnt != xfree_cnt)
			exit(-1);
		xalloc_cnt = xfree_cnt = 0;
	}

	free(iocbs);
	free(events);
	free(iocb_list);
	opio_free(&ctx);

	return 0;
}
#endif
