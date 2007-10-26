/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>

#define TAPDISK
#include "tapdisk.h"
#include "vhd.h"

#if 1
#define DFPRINTF(_f, _a...) fprintf (stdout, _f , ## _a)
#else
#define DFPRINTF(_f, _a...) ((void)0)
#endif

#define MAX_AIO_REQUESTS  TAPDISK_DATA_REQUESTS

struct preq {
	char         *buf;
	uint64_t      sec;
};

struct vhd_context {
	int           bat_idx;

	char         *bm;
	int           bm_idx;
	int           bm_dirty;
	uint64_t      bm_offset;

	int           error;
	int           preqs;
	int           running;
	uint64_t      cur_sec;

	int           child_fd;

	int           write_queue_cnt;
	struct preq   write_queue[MAX_AIO_REQUESTS];

	char         *memory;
	char         *free_buffer_list[MAX_AIO_REQUESTS];
	int           free_buffer_cnt;

	struct vhd_info     info;
	struct disk_driver *child;
	struct disk_driver *parent;
};

static inline int
test_bit (int nr, volatile u32 *addr)
{
	return (((u32 *)addr)[nr >> 5] >> (nr & 31)) & 1;
}

static int
init_buffers(struct vhd_context *ctx)
{
	int i;

	if (posix_memalign((void **)&ctx->memory, DEFAULT_SECTOR_SIZE,
			   MAX_AIO_REQUESTS * DEFAULT_SECTOR_SIZE))
		return -ENOMEM;

	for (i = 0; i < MAX_AIO_REQUESTS; i++)
		ctx->free_buffer_list[i] = 
			&ctx->memory[i * DEFAULT_SECTOR_SIZE];

	ctx->free_buffer_cnt = MAX_AIO_REQUESTS;
	return 0;
}

static inline void
free_buffers(struct vhd_context *ctx)
{
	free(ctx->memory);
}

static inline char *
get_buffer(struct vhd_context *ctx)
{
	if (ctx->free_buffer_cnt > 0)
		return ctx->free_buffer_list[--ctx->free_buffer_cnt];
	return NULL;
}

static inline void
put_buffer(struct vhd_context *ctx, char *buf)
{
	ctx->free_buffer_list[ctx->free_buffer_cnt++] = buf;
}

static inline int
buffer_to_idx(struct vhd_context *ctx, char *buf)
{
	return ((buf - ctx->memory) / DEFAULT_SECTOR_SIZE);
}

static inline char *
idx_to_buffer(struct vhd_context *ctx, int idx)
{
	return &ctx->memory[idx * DEFAULT_SECTOR_SIZE];
}

static int
init_ctx(struct vhd_context *ctx, 
	 struct disk_driver *child, struct disk_driver *parent)
{
	struct tqueue *queue = &child->td_state->queue;

	memset(ctx, 0, sizeof(struct vhd_context));
	ctx->child  = child;
	ctx->parent = parent;

	ctx->error = tapdisk_init_queue(queue,
					MAX_AIO_REQUESTS + 
					child->drv->private_iocbs +
					parent->drv->private_iocbs,
					0, NULL, NULL);
	if (ctx->error)
		return ctx->error;

	ctx->error = vhd_get_info(child, &ctx->info);
	if (ctx->error)
		return ctx->error;

	ctx->child_fd = open(child->name, O_RDWR);
	if (ctx->child_fd == -1) {
		ctx->error = -errno;
		return ctx->error;
	}

	ctx->running = 1;
	ctx->bm = malloc(ctx->info.spb >> 3);
	if (!ctx->bm) {
		ctx->error = -ENOMEM;
		return ctx->error;
	}

	ctx->error = init_buffers(ctx);
	if (ctx->error)
		return ctx->error;

	return 0;
}

static int
open_images(char *path, struct disk_driver *child, struct disk_driver *parent)
{
	int err;
	struct disk_id id;

	if ((err = child->drv->td_open(child, path, TD_OPEN_RDONLY))) {
		DFPRINTF("error opening %s\n", path);
		return -err;
	}

	if ((err = child->drv->td_get_parent_id(child, &id))) {
		DFPRINTF("%s does not have a parent\n", path);
		goto fail_child;
	}

	if (id.drivertype > MAX_DISK_TYPES ||
	    !dtypes[id.drivertype]->drv || !id.name) {
		DFPRINTF("error getting parent id for %s\n", path);
		err = -EINVAL;
		goto fail_child;
	}
	
	parent->drv     = dtypes[id.drivertype]->drv;
	parent->private = malloc(parent->drv->private_data_size);
	if (!parent->private) {
		err = -ENOMEM;
		goto fail_child;
	}

	if ((err = parent->drv->td_open(parent, id.name, 0))) {
		DFPRINTF("error opening parent %s\n", id.name);
		goto fail_parent;
	}

	free(id.name);

	return 0;

 fail_parent:
	free(parent->private);
 fail_child:
	child->drv->td_close(child);
	return err;
}

static int
process_write(struct disk_driver *parent, int res,
	      uint64_t sec, int nr_secs, int idx, void *private)
{
	struct vhd_context *ctx = (struct vhd_context *)private;
	char *buf = idx_to_buffer(ctx, idx);

	if (res == -EBUSY)
		return -EBUSY;

	--ctx->preqs;
	put_buffer(ctx, buf);

	if (res) {
		DFPRINTF("ERROR writing sector %" PRIu64 ", res %d\n", sec, res);
		ctx->error = res;
	}

	return 0;
}

static int
process_read(struct disk_driver *child, int res,
	     uint64_t sec, int nr_secs, int idx, void *private)
{
	struct vhd_context *ctx = (struct vhd_context *)private;
	char *buf = idx_to_buffer(ctx, idx);

	if (res) {
		DFPRINTF("ERROR reading sector %" PRIu64 ", res: %d\n", sec, res);
		ctx->error = res;
		--ctx->preqs;
		put_buffer(ctx, buf);
		return 0;
	}

	res = ctx->parent->drv->td_queue_write(ctx->parent, sec, 1, buf,
					       process_write, idx, (void *)ctx);

	if (res == -EBUSY) {
		struct preq *preq = &ctx->write_queue[ctx->write_queue_cnt++];
		preq->buf = buf;
		preq->sec = sec;
	} else if (res < 0) {
		DFPRINTF("ERROR writing sector %" PRIu64 "\n", sec);
		ctx->error = res;
		--ctx->preqs;
		put_buffer(ctx, buf);
		return 0;
	}

	return 0;
}

static void
flush_write_queue(struct vhd_context *ctx)
{
	int ret, idx;
	struct preq *preq;

	while (ctx->write_queue_cnt > 0) {
		preq = &ctx->write_queue[--ctx->write_queue_cnt];
		idx  = buffer_to_idx(ctx, preq->buf);
		ret  = ctx->parent->drv->td_queue_write(ctx->parent, preq->sec,
							1, preq->buf,
							process_write,
							idx, (void *)ctx);
		if (ret == -EBUSY) {
			ctx->write_queue_cnt++;
			return;
		} else if (ret < 0) {
			DFPRINTF("ERROR writing sector %" PRIu64 "\n", preq->sec);
			ctx->error = ret;
			--ctx->preqs;
			put_buffer(ctx, preq->buf);
			return;
		}
	}
}

static int
write_current_bitmap(struct vhd_context *ctx)
{
	uint64_t offset = ctx->bm_offset;
	int bm_size = ctx->info.spb >> 3;

	if (!ctx->bm_dirty)
		return 0;

	if (ctx->preqs)
		return -EAGAIN;

	if (ctx->error)
		return ctx->error;

	if (lseek64(ctx->child_fd, offset << 9, SEEK_SET) == (off64_t)-1)
		goto error;

	memset(ctx->bm, 0, bm_size);
	if (write(ctx->child_fd, ctx->bm, bm_size) < bm_size)
		goto error;

	return 0;

 error:
	ctx->error = (errno ? -errno : -EIO);
	return ctx->error;
}

static int
read_next_bitmap(struct vhd_context *ctx)
{
	uint64_t offset;
	int bm_size = ctx->info.spb >> 3;

	if (write_current_bitmap(ctx))
		return 1;

	do {
		if (++ctx->bat_idx >= ctx->info.bat_entries) {
			ctx->running = 0;
			return 1;
		}
		offset = ctx->info.bat[ctx->bat_idx];
	} while (offset == DD_BLK_UNUSED);

	ctx->bm_idx    = 0;
	ctx->cur_sec   = ctx->bat_idx * ctx->info.spb;
	ctx->bm_dirty  = 0;
	ctx->bm_offset = offset;

	if (lseek64(ctx->child_fd, offset << 9, SEEK_SET) == (off64_t)-1)
		goto error;

	if (read(ctx->child_fd, ctx->bm, bm_size) < bm_size)
		goto error;

	return 0;

 error:
	ctx->error = (errno ? -errno : -EIO);
	return ctx->error;
}

static inline void
ctx_inc_bm(struct vhd_context *ctx)
{
	++ctx->bm_idx;
	++ctx->cur_sec;
}

static int
process(struct vhd_context *ctx)
{
	int   ret;
	char *buf;

	if (ctx->error || ctx->bat_idx >= ctx->info.bat_entries)
		return ctx->error;

	while (ctx->free_buffer_cnt) {

		if (ctx->bm_idx >= ctx->info.spb)
			if (read_next_bitmap(ctx))
				return 0;

		while (ctx->bm_idx < ctx->info.spb &&
		       !test_bit(ctx->bm_idx, (u32 *)ctx->bm)) 
			ctx_inc_bm(ctx);

		if (ctx->bm_idx < ctx->info.spb &&
		    test_bit(ctx->bm_idx, (u32 *)ctx->bm)) {
			buf = get_buffer(ctx);
			ret = ctx->child->drv->td_queue_read(ctx->child,
							     ctx->cur_sec, 1, 
							     buf, process_read,
							     buffer_to_idx(ctx, buf),
							     (void *)ctx);
			if (ret < 0) {
				put_buffer(ctx, buf);
				if (ret == -EBUSY)
					return 0;

				DFPRINTF("ERROR reading %" PRIu64 "\n", ctx->cur_sec);
				ctx->error = ret;
				return ret;
			}
			ctx->bm_dirty = 1;
			++ctx->preqs;
		}

		ctx_inc_bm(ctx);
	}

	return 0;
}

int
vhd_coalesce(char *path)
{
	int    ret;
	fd_set readfds;
	struct td_state s;
	struct tqueue *queue;
	struct vhd_context ctx;
	struct disk_driver child, parent;

	memset(&s,      0, sizeof(struct td_state));
	memset(&child,  0, sizeof(struct disk_driver));
	memset(&parent, 0, sizeof(struct disk_driver));

	queue           = &s.queue;
	child.td_state  = &s;
	child.name      = path;
	child.drv       = dtypes[DISK_TYPE_VHD]->drv;
	child.private   = malloc(child.drv->private_data_size);
	if (!child.private)
		return -1;

	parent.td_state = &s;
	parent.private  = NULL;

	ctx.error = open_images(path, &child, &parent);
	if (ctx.error)
		return ctx.error;

	if (init_ctx(&ctx, &child, &parent))
		goto done;

	ctx.bat_idx = -1;
	read_next_bitmap(&ctx);
	process(&ctx);
	tapdisk_submit_all_tiocbs(queue);

	while (ctx.running || ctx.preqs) {
		FD_ZERO(&readfds);
		FD_SET(queue->poll_fd, &readfds);
		ret = select(queue->poll_fd + 1,
			     &readfds, NULL, NULL, NULL);

		if (ret > 0) {
			if (FD_ISSET(queue->poll_fd, &readfds))
				tapdisk_complete_tiocbs(queue);

			flush_write_queue(&ctx);
			process(&ctx);
			tapdisk_submit_all_tiocbs(queue);
		}

		if (ctx.error && !ctx.preqs)
			break;
	}

 done:
	close(ctx.child_fd);
	child.drv->td_close(&child);
	parent.drv->td_close(&parent);
	tapdisk_free_queue(queue);
	free(child.private);
	free(parent.private);
	free(ctx.info.bat);
	free(ctx.bm);
	free_buffers(&ctx);

	return ctx.error;
}
