/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 */

#ifndef __IO_OPTIMIZE_H__
#define __IO_OPTIMIZE_H__

#include <libaio.h>

struct opio;

struct opio_list {
	struct opio        *head;
	struct opio        *tail;
};

struct opio {
	char               *buf;
	unsigned long       nbytes;
	long long           offset;
	void               *data;
	struct iocb        *iocb;
	struct io_event     event;
	struct opio        *head;
	struct opio        *next;
	struct opio_list    list;
};

struct opioctx {
	int                 num_opios;
	int                 free_opio_cnt;
	struct opio        *opios;
	struct opio       **free_opios;
	struct iocb       **iocb_queue;
	struct io_event    *event_queue;
};

int opio_init(struct opioctx *ctx, int num_iocbs);
void opio_free(struct opioctx *ctx);
int io_merge(struct opioctx *ctx, struct iocb **queue, int num);
int io_split(struct opioctx *ctx, struct io_event *events, int num);
int io_expand_iocbs(struct opioctx *ctx, struct iocb **queue, int idx, int num);

#endif
