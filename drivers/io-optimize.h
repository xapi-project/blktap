/* 
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
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
