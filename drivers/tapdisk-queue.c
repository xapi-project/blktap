/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#include <errno.h>
#include <libaio.h>

#include "tapdisk.h"
#include "profile.h"
#include "atomicio.h"
#include "tapdisk-filter.h"

static struct tlog *log;
#define DBG(_f, _a...) tlog_write(log, TLOG_WARN, _f, ##_a)

/*
 * We used a kernel patch to return an fd associated with the AIO context
 * so that we can concurrently poll on synchronous and async descriptors.
 * This is signalled by passing 1 as the io context to io_setup.
 */
#define REQUEST_ASYNC_FD 1

static inline void
queue_tiocb(struct tqueue *queue, struct tiocb *tiocb)
{
	struct iocb *iocb = &tiocb->iocb;

	if (queue->queued) {
		struct tiocb *prev = (struct tiocb *)
			queue->iocbs[queue->queued - 1]->data;
		prev->next = tiocb;
	}

	queue->iocbs[queue->queued++] = iocb;
}

static inline int
deferred_tiocbs(struct tqueue *queue)
{
	return (queue->deferred.head != NULL);
}

static inline void
defer_tiocb(struct tqueue *queue, struct tiocb *tiocb)
{
	struct tlist *list = &queue->deferred;

	if (!list->head)
		list->head = list->tail = tiocb;
	else
		list->tail = list->tail->next = tiocb;

	queue->tiocbs_deferred++;
}

static inline void
queue_deferred_tiocb(struct tqueue *queue)
{
	struct tlist *list = &queue->deferred;

	if (list->head) {
		struct tiocb *tiocb = list->head;

		list->head = tiocb->next;
		if (!list->head)
			list->tail = NULL;

		queue_tiocb(queue, tiocb);
		queue->tiocbs_deferred--;
	}
}

static inline void
queue_deferred_tiocbs(struct tqueue *queue)
{
	while (!tapdisk_queue_full(queue) && deferred_tiocbs(queue))
		queue_deferred_tiocb(queue);
}

/*
 * td_complete may queue more tiocbs
 */
static void
complete_tiocb(struct tqueue *queue, struct tiocb *tiocb, unsigned long res)
{
	int err;
	struct iocb *iocb = &tiocb->iocb;
	struct disk_driver *dd = tiocb->dd;

	if (res == iocb->u.c.nbytes)
		err = 0;
	else if ((int)res < 0)
		err = (int)res;
	else
		err = -EIO;

	dd->drv->td_complete(dd, tiocb, err);
}

static int
cancel_tiocbs(struct tqueue *queue, int err)
{
	int queued;
	struct tiocb *tiocb;

	if (!queue->queued)
		return 0;

	/* 
	 * td_complete may queue more tiocbs, which
	 * will overwrite the contents of queue->iocbs.
	 * use a private linked list to keep track
	 * of the tiocbs we're cancelling. 
	 */
	tiocb  = (struct tiocb *)queue->iocbs[0]->data;
	queued = queue->queued;
	queue->queued = 0;

	for (; tiocb != NULL; tiocb = tiocb->next)
		complete_tiocb(queue, tiocb, err);

	return queued;
}

static int
fail_tiocbs(struct tqueue *queue, int succeeded, int total, int err)
{
	TAP_ERROR(err, "io_submit error: %d of %d failed",
		  total - succeeded, total);

	/* take any non-submitted, merged iocbs 
	 * off of the queue, split them, and fail them */
	queue->queued = io_expand_iocbs(&queue->opioctx,
					queue->iocbs, succeeded, total);

	return cancel_tiocbs(queue, err);
}

static inline ssize_t
iocb_rw(struct iocb *iocb)
{
	int fd        = iocb->aio_fildes;
	char *buf     = iocb->u.c.buf;
	long long off = iocb->u.c.offset;
	size_t size   = iocb->u.c.nbytes;
	ssize_t (*func)(int, void *, size_t) = 
		(iocb->aio_lio_opcode == IO_CMD_PWRITE ? vwrite : read);

	if (lseek64(fd, off, SEEK_SET) == (off64_t)-1)
		return -errno;
	
	if (atomicio(func, fd, buf, size) != size)
		return -errno;

	return size;
}

static int
io_synchronous_rw(struct tqueue *queue)
{
	int i, merged, split;
	struct iocb *iocb;
	struct tiocb *tiocb;
	struct io_event *ep;

	if (!queue->queued)
		return 0;

	tapdisk_filter_iocbs(queue->filter, queue->iocbs, queue->queued);
	merged = io_merge(&queue->opioctx, queue->iocbs, queue->queued);

	queue->queued = 0;

	for (i = 0; i < merged; i++) {
		ep      = queue->aio_events + i;
		iocb    = queue->iocbs[i];
		ep->obj = iocb;
		ep->res = iocb_rw(iocb);
	}

	split = io_split(&queue->opioctx, queue->aio_events, merged);
	tapdisk_filter_events(queue->filter, queue->aio_events, split);

	for (i = split, ep = queue->aio_events; i-- > 0; ep++) {
		iocb  = ep->obj;
		tiocb = (struct tiocb *)iocb->data;
		complete_tiocb(queue, tiocb, ep->res);
	}

	queue_deferred_tiocbs(queue);

	return split;
}

int
tapdisk_init_queue(struct tqueue *queue, int size, int sync,
		   struct tlog *tlog, struct tfilter *filter)
{
	int i, err;

	memset(queue, 0, sizeof(struct tqueue));

	queue->size   = size;
	queue->sync   = sync;
	queue->filter = filter;
	queue->log    = log = tlog;

	if (sync) {
		/* set up a pipe so we can return
		 * a poll fd that won't fire. */
		if (pipe(queue->dummy_pipe))
			return -errno;
		queue->poll_fd = queue->dummy_pipe[0];
	} else {
		queue->aio_ctx = (io_context_t)REQUEST_ASYNC_FD;
		queue->poll_fd = io_setup(size, &queue->aio_ctx);

		if (queue->poll_fd < 0) {
			if (queue->poll_fd == -EAGAIN)
				DPRINTF("Couldn't setup AIO context.  If you "
					"are trying to concurrently use a "
					"large number of blktap-based disks, "
					"you may need to increase the "
					"system-wide aio request limit. "
					"(e.g. 'echo 1048576 > /proc/sys/fs/"
					"aio-max-nr')\n");
			else
				DPRINTF("Couldn't get fd for AIO poll "
					"support.  This is probably because "
					"your kernel does not have the "
					"aio-poll patch applied.\n");
			return queue->poll_fd;
		}
	}

	err               = -ENOMEM;
	queue->iocbs      = calloc(size, sizeof(struct iocb *));
	queue->aio_events = calloc(size, sizeof(struct io_event));
	if (!queue->iocbs || !queue->aio_events)
		goto fail;

	err = opio_init(&queue->opioctx, size, tlog);
	if (err)
		goto fail;

	return 0;

 fail:
	tapdisk_free_queue(queue);
	return err;
}

void
tapdisk_free_queue(struct tqueue *queue)
{
	if (queue->sync) {
		close(queue->dummy_pipe[0]);
		close(queue->dummy_pipe[1]);
	} else
		io_destroy(queue->aio_ctx);

	free(queue->iocbs);
	free(queue->aio_events);
	opio_free(&queue->opioctx);
}

void 
tapdisk_debug_queue(struct tqueue *queue)
{
	struct tiocb *tiocb = queue->deferred.head;

	DBG("TAPDISK QUEUE:\n");
	DBG("size: %d, sync: %d, queued: %d, iocbs_pending: %d, "
	    "tiocbs_pending: %d, tiocbs_deferred: %d\n",
	    queue->size, queue->sync, queue->queued, queue->iocbs_pending,
	    queue->tiocbs_pending, queue->tiocbs_deferred);

	if (tiocb) {
		DBG("deferred:\n");
		for (; tiocb != NULL; tiocb = tiocb->next) {
			struct iocb *io = &tiocb->iocb;
			DBG("%s of %lu bytes at %lld\n",
			    (io->aio_lio_opcode == IO_CMD_PWRITE ?
			     "write" : "read"),
			    io->u.c.nbytes, io->u.c.offset);
		}
	}
}

void
tapdisk_prep_tiocb(struct tiocb *tiocb, struct disk_driver *dd,
		   int fd, int rw, char *buf, size_t size,
		   long long offset, void *data)
{
	struct iocb *iocb = &tiocb->iocb;

	if (rw)
		io_prep_pwrite(iocb, fd, buf, size, offset);
	else
		io_prep_pread(iocb, fd, buf, size, offset);

	iocb->data  = tiocb;
	tiocb->data = data;
	tiocb->next = NULL;
	tiocb->dd   = dd;
}

void
tapdisk_queue_tiocb(struct tqueue *queue, struct tiocb *tiocb)
{
	if (!tapdisk_queue_full(queue))
		queue_tiocb(queue, tiocb);
	else
		defer_tiocb(queue, tiocb);
}

/*
 * fail_tiocbs may queue more tiocbs
 */
int
tapdisk_submit_tiocbs(struct tqueue *queue)
{
	int merged, submitted, err = 0;

	if (!queue->queued)
		return 0;

	if (queue->sync)
		return io_synchronous_rw(queue);

	tapdisk_filter_iocbs(queue->filter, queue->iocbs, queue->queued);
	merged    = io_merge(&queue->opioctx, queue->iocbs, queue->queued);
	submitted = io_submit(queue->aio_ctx, merged, queue->iocbs);

	if (submitted < 0) {
		err = submitted;
		submitted = 0;
	} else if (submitted < merged)
		err = -EIO;

	queue->iocbs_pending  += submitted;
	queue->tiocbs_pending += queue->queued;
	queue->queued          = 0;

	if (err)
		queue->tiocbs_pending -= 
			fail_tiocbs(queue, submitted, merged, err);

	return submitted;
}

int
tapdisk_submit_all_tiocbs(struct tqueue *queue)
{
	int submitted = 0;

	do {
		submitted += tapdisk_submit_tiocbs(queue);
	} while (!tapdisk_queue_empty(queue));

	return submitted;
}

int
tapdisk_complete_tiocbs(struct tqueue *queue)
{
	int i, ret, split;
	struct iocb *iocb;
	struct tiocb *tiocb;
	struct io_event *ep;

	ret   = io_getevents(queue->aio_ctx, 0,
			     queue->size, queue->aio_events, NULL);
	split = io_split(&queue->opioctx, queue->aio_events, ret);
	tapdisk_filter_events(queue->filter, queue->aio_events, split);

	queue->iocbs_pending  -= ret;
	queue->tiocbs_pending -= split;

	for (i = split, ep = queue->aio_events; i-- > 0; ep++) {
		iocb  = ep->obj;
		tiocb = (struct tiocb *)iocb->data;
		complete_tiocb(queue, tiocb, ep->res);
	}

	queue_deferred_tiocbs(queue);

	return split;
}

/*
 * cancel_tiocbs may queue more tiocbs
 */
int
tapdisk_cancel_tiocbs(struct tqueue *queue)
{
	return cancel_tiocbs(queue, -EIO);
}

int
tapdisk_cancel_all_tiocbs(struct tqueue *queue)
{
	int cancelled = 0;

	do {
		cancelled += tapdisk_cancel_tiocbs(queue);
	} while (!tapdisk_queue_empty(queue));

	return cancelled;
}

void
td_queue_tiocb(struct disk_driver *dd, struct tiocb *tiocb)
{
	struct td_state *s = dd->td_state;
	struct tqueue   *q = &s->queue;
	tapdisk_queue_tiocb(q, tiocb);
}

void
td_prep_read(struct tiocb *tiocb, struct disk_driver *dd, int fd,
	     char *buf, size_t bytes, long long offset, void *data)
{
	tapdisk_prep_tiocb(tiocb, dd, fd, 0, buf, bytes, offset, data);
}

void
td_prep_write(struct tiocb *tiocb, struct disk_driver *dd, int fd,
	      char *buf, size_t bytes, long long offset, void *data)
{
	tapdisk_prep_tiocb(tiocb, dd, fd, 1, buf, bytes, offset, data);
}
