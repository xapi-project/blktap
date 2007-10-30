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

#define TAPDISK
#include "tapdisk.h"
#include "vhd.h"

#if 1
#define DFPRINTF(_f, _a...)  fprintf (stdout, _f , ## _a)
#else
#define DFPRINTF(_f, _a...)  ((void)0)
#endif

#define SECTOR_SHIFT         9

struct vhd_context {
	char                *buf;
	uint64_t             sec;

	int                  error;
	int                  request_pending;
	unsigned long        blk_size;

	struct vhd_info      info;
	struct disk_driver  *dd;
};

static void
free_vhd_context(struct vhd_context *ctx)
{
	free(ctx->buf);
	free(ctx->info.bat);
	tapdisk_free_queue(&ctx->dd->td_state->queue);
}

static int
init_vhd_context(struct vhd_context *ctx, struct disk_driver *dd)
{
	struct tqueue *queue = &dd->td_state->queue;

	memset(ctx, 0, sizeof(struct vhd_context));

	ctx->dd       = dd;
	ctx->error    = vhd_get_bat(dd, &(ctx->info));
	if (ctx->error)
		return ctx->error;

	ctx->error    = tapdisk_init_queue(queue,
					   TAPDISK_DATA_REQUESTS + 
					   dd->drv->private_iocbs,
					   0, NULL, NULL);
	if (ctx->error)
		goto fail;

	ctx->blk_size = (ctx->info.spb << SECTOR_SHIFT);
	ctx->error    = posix_memalign((void **)&ctx->buf, 512, ctx->blk_size);
	if (ctx->error)
		goto fail;

	memset(ctx->buf, 0, ctx->blk_size);
	return 0;

 fail:
	free_vhd_context(ctx);
	return ctx->error;
}

static inline unsigned long
blk_to_sec(struct vhd_context *ctx, unsigned long long blk)
{
	return blk * ctx->info.spb;
}

static void
wait_for_responses(struct vhd_context *ctx)
{
	int ret;
	fd_set readfds;
	struct disk_driver *dd = ctx->dd;
	struct tqueue *queue   = &dd->td_state->queue;

	if (!ctx->request_pending || ctx->error)
		return;

	FD_ZERO(&readfds);
	FD_SET(queue->poll_fd, &readfds);
	ret = select(queue->poll_fd + 1, &readfds, NULL, NULL, NULL);

	if (ret > 0)
		if (FD_ISSET(queue->poll_fd, &readfds))
			tapdisk_complete_tiocbs(queue);
}

static int
finish_write(struct disk_driver *dd, int res, 
	     uint64_t sec, int nr_secs, int idx, void *private)
{
	struct vhd_context *ctx = (struct vhd_context *)private;

	ctx->error = res;
	ctx->request_pending = 0;

	return res;
}

static int
fill_block(struct vhd_context *ctx, unsigned long long blk)
{
	int ret;
	unsigned long secs;
	unsigned long long sec;
	struct disk_driver *dd;
	struct tqueue *queue;

	/*
	 * presently, vhd preallocates entire 2MB blocks,
	 * so if the block is allocated, we don't need to fill it.
	 * NB: this will need to change if vhd ceases to preallocate.
	 */
	if (ctx->info.bat[blk] != DD_BLK_UNUSED)
		return 0;
	
 again:
	dd    = ctx->dd;
	sec   = blk_to_sec(ctx, blk);
	secs  = ctx->blk_size >> SECTOR_SHIFT;
	queue = &dd->td_state->queue;
	ctx->request_pending = 1;
	ctx->error = 0;

	ctx->error = dd->drv->td_queue_write(dd, sec, secs, ctx->buf, 
					     finish_write, 0, (void *)ctx);

	if (!ctx->error) {
		do {
			tapdisk_submit_all_tiocbs(queue);
			wait_for_responses(ctx);
		} while (ctx->request_pending);
	}

	if (ctx->error == -EBUSY)
		goto again;

	return ctx->error;
}

int
vhd_fill(char *path)
{
	int err;
	unsigned long long i;
	struct td_state s;
	struct vhd_context ctx;
	struct disk_driver dd;
	struct disk_id id;

	memset(&s,   0, sizeof(struct td_state));
	memset(&dd,  0, sizeof(struct disk_driver));
	memset(&ctx, 0, sizeof(struct vhd_context));

	dd.td_state = &s;
	dd.name     = path;
	dd.drv      = dtypes[DISK_TYPE_VHD]->drv;
	dd.private  = malloc(dd.drv->private_data_size);
	if (!dd.private)
		return -ENOMEM;

	err = dd.drv->td_open(&dd, path, 0);
	if (err) {
		printf("error opening %s: %d\n", path, err);
		free(dd.private);
		return err;
	}

	err = dd.drv->td_get_parent_id(&dd, &id);
	if (err != TD_NO_PARENT) {
		if (!err) {
			printf("filling CoW VHDs not supported\n");
			free(id.name);
			ctx.error = -EINVAL;
		} else
			ctx.error = err;
		goto done;
	}

	init_vhd_context(&ctx, &dd);
	if (ctx.error)
		goto done;

	for (i = 0; i < ctx.info.bat_entries && !ctx.error; i++)
		fill_block(&ctx, i);

	while (ctx.request_pending && !ctx.error)
		wait_for_responses(&ctx);

 done:
	dd.drv->td_close(&dd);
	free(dd.private);
	free_vhd_context(&ctx);

	return ctx.error;
}
