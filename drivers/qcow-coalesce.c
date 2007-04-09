#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define TAPDISK
#include "tapdisk.h"

#if 1
#define DFPRINTF(_f, _a...) fprintf ( stdout, _f , ## _a )
#else
#define DFPRINTF(_f, _a...) ((void)0)
#endif

#define MAX_AIO_REQUESTS (MAX_REQUESTS * MAX_SEGMENTS_PER_REQ)

struct preq {
	char         *buf;
	uint64_t      sec;
};

struct qcow_context {
	int           l1_idx;

	uint64_t     *l2;
	int           l2_idx;
	uint64_t      l2_offset;

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

	struct qcow_info    info;
	struct disk_driver *child;
	struct disk_driver *parent;
};

static int
init_buffers(struct qcow_context *ctx)
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
free_buffers(struct qcow_context *ctx)
{
	free(ctx->memory);
}

static inline char *
get_buffer(struct qcow_context *ctx)
{
	if (ctx->free_buffer_cnt > 0)
		return ctx->free_buffer_list[--ctx->free_buffer_cnt];
	return NULL;
}

static inline void
put_buffer(struct qcow_context *ctx, char *buf)
{
	ctx->free_buffer_list[ctx->free_buffer_cnt++] = buf;
}

static inline int
buffer_to_idx(struct qcow_context *ctx, char *buf)
{
	return ((buf - ctx->memory) / DEFAULT_SECTOR_SIZE);
}

static inline char *
idx_to_buffer(struct qcow_context *ctx, int idx)
{
	return &ctx->memory[idx * DEFAULT_SECTOR_SIZE];
}

static int
init_ctx(struct qcow_context *ctx, 
	 struct disk_driver *child, struct disk_driver *parent)
{
	memset(ctx, 0, sizeof(struct qcow_context));
	ctx->child  = child;
	ctx->parent = parent;

	if (qcow_get_info(child, &ctx->info))
		return -1;

	ctx->child_fd = open(child->name, O_RDWR);
	if (ctx->child_fd == -1)
		return -1;

	ctx->running = 1;
	ctx->l2 = malloc(ctx->info.l2_size * sizeof(uint64_t));
	if (!ctx->l2)
		return -1;

	if (init_buffers(ctx))
		return -1;

	return 0;
}

static int
open_images(char *path, struct disk_driver *child, struct disk_driver *parent)
{
	struct disk_id id;

	if (child->drv->td_open(child, path, TD_RDONLY)) {
		DFPRINTF("error opening %s\n", path);
		return -1;
	}

	if (child->drv->td_get_parent_id(child, &id)) {
		DFPRINTF("%s does not have a parent\n", path);
		goto fail_child;
	}

	if (id.drivertype > MAX_DISK_TYPES ||
	    !dtypes[id.drivertype]->drv || !id.name) {
		DFPRINTF("error getting parent id for %s\n", path);
		goto fail_child;
	}
	
	parent->drv     = dtypes[id.drivertype]->drv;
	parent->private = malloc(parent->drv->private_data_size);
	if (!parent->private)
		goto fail_child;

	if (parent->drv->td_open(parent, id.name, 0)) {
		DFPRINTF("error opening parent %s\n", id.name);
		goto fail_child;
	}

	child->name  = strdup(path);
	parent->name = strdup(id.name);

	free(id.name);

	return 0;

 fail_parent:
	parent->drv->td_close(parent);
 fail_child:
	child->drv->td_close(child);
	return -1;
}

static int
process_write(struct disk_driver *parent, int res,
	      uint64_t sec, int nr_secs, int idx, void *private)
{
	struct qcow_context *ctx = (struct qcow_context *)private;
	char *buf = idx_to_buffer(ctx, idx);

	if (res == -EBUSY)
		return -EBUSY;

	--ctx->preqs;
	put_buffer(ctx, buf);

	if (res) {
		DFPRINTF("ERROR writing sector %llu, res %d\n", sec, res);
		ctx->error = res;
	}

	return 0;
}

static int
process_read(struct disk_driver *child, int res,
	     uint64_t sec, int nr_secs, int idx, void *private)
{
	struct qcow_context *ctx = (struct qcow_context *)private;
	char *buf = idx_to_buffer(ctx, idx);

	if (res) {
		DFPRINTF("ERROR reading sector %llu, res: %d\n", sec, res);
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
		DFPRINTF("ERROR writing sector %llu\n", sec);
		ctx->error = res;
		--ctx->preqs;
		put_buffer(ctx, buf);
		return 0;
	}

	return 0;
}

static void
flush_write_queue(struct qcow_context *ctx)
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
			DFPRINTF("ERROR writing sector %llu\n", preq->sec);
			ctx->error = ret;
			--ctx->preqs;
			put_buffer(ctx, preq->buf);
			return;
		}
	}
}

static int
write_current_l2_table(struct qcow_context *ctx)
{
	uint64_t offset = ctx->l2_offset;
	int size = ctx->info.l2_size * sizeof(uint64_t);

	if (!offset)
		return 0;

	if (ctx->preqs)
		return -EAGAIN;

	if (lseek64(ctx->child_fd, offset, SEEK_SET) == (off64_t)-1)
		goto error;

	memset(ctx->l2, 0, size);
	if (write(ctx->child_fd, (char *)ctx->l2, size) < size)
		goto error;

	return 0;

 error:
	ctx->error = (errno ? -errno : -EIO);
	DFPRINTF("%s: error: %d\n", __func__, ctx->error);
	return ctx->error;
}

static int
read_next_l2_table(struct qcow_context *ctx)
{
	uint64_t offset;
	int size = ctx->info.l2_size * sizeof(uint64_t);

	if (write_current_l2_table(ctx))
		return 1;

	do {
		if (++ctx->l1_idx >= ctx->info.l1_size) {
			ctx->running = 0;
			return 1;
		}
		offset = ctx->info.l1[ctx->l1_idx];
	} while (!offset);

	ctx->l2_idx    = 0;
	ctx->cur_sec   = ctx->l1_idx * ctx->info.l2_size;
	ctx->l2_offset = offset;

	if (lseek64(ctx->child_fd, offset, SEEK_SET) == (off64_t)-1)
		goto error;

	if (read(ctx->child_fd, (char *)ctx->l2, size) < size)
		goto error;

	return 0;

 error:
	ctx->error = (errno ? -errno : -EIO);
	DFPRINTF("%s: error: %d\n", __func__, ctx->error);
	return ctx->error;
}

static inline void
ctx_inc_l2(struct qcow_context *ctx)
{
	++ctx->l2_idx;
	++ctx->cur_sec;
}

static int
process(struct qcow_context *ctx)
{
	int   ret;
	char *buf;

	if (ctx->error || ctx->l1_idx >= ctx->info.l1_size)
		return ctx->error;

	while (ctx->free_buffer_cnt) {

		if (ctx->l2_idx >= ctx->info.l2_size)
			if (read_next_l2_table(ctx))
				return 0;

		while (ctx->l2_idx < ctx->info.l2_size && !ctx->l2[ctx->l2_idx])
			ctx_inc_l2(ctx);

		if (ctx->l2_idx < ctx->info.l2_size && ctx->l2[ctx->l2_idx]) {
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

				DFPRINTF("ERROR reading %llu\n", ctx->cur_sec);
				ctx->error = ret;
				return ret;
			}
			++ctx->preqs;
		}

		ctx_inc_l2(ctx);
	}

	return 0;
}

int
qcow_coalesce(char *path)
{
	fd_set readfds;
	int    maxfds, ret;
	struct td_state s;
	struct qcow_context ctx;
	struct disk_driver child, parent;

	child.td_state  = &s;
	child.name      = path;
	child.drv       = dtypes[DISK_TYPE_QCOW]->drv;
	child.private   = malloc(child.drv->private_data_size);
	if (!child.private)
		return -1;

	parent.td_state = &s;
	parent.private  = NULL;

	if (open_images(path, &child, &parent))
		return -1;

	if (init_ctx(&ctx, &child, &parent)) {
		ctx.error = -EIO;
		goto done;
	}

	maxfds = (child.io_fd[READ] > parent.io_fd[READ] ?
		  child.io_fd[READ] : parent.io_fd[READ]);

	ctx.l1_idx = -1;
	read_next_l2_table(&ctx);
	process(&ctx);
	child.drv->td_submit(&child);

	while (ctx.running || ctx.preqs) {
		FD_ZERO(&readfds);
		FD_SET(child.io_fd[READ], &readfds);
		FD_SET(parent.io_fd[READ], &readfds);
		ret = select(maxfds + 1, &readfds, NULL, NULL, NULL);

		if (ret > 0) {
			if (FD_ISSET(child.io_fd[READ], &readfds))
				child.drv->td_do_callbacks(&child, 0);
			if (FD_ISSET(parent.io_fd[READ], &readfds))
				parent.drv->td_do_callbacks(&parent, 0);

			flush_write_queue(&ctx);

			process(&ctx);
			child.drv->td_submit(&child);
			parent.drv->td_submit(&parent);
		}

		if (ctx.error && !ctx.preqs)
			break;
	}

 done:
	close(ctx.child_fd);
	child.drv->td_close(&child);
	parent.drv->td_close(&parent);
	free(child.private);
	free(parent.private);
	free(ctx.info.l1);
	free(ctx.l2);
	free_buffers(&ctx);

	return ctx.error;
}
