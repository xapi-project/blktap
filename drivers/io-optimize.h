/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __IO_OPTIMIZE_H__
#define __IO_OPTIMIZE_H__

#include "debug.h"
#include <libaio.h>
#include <sys/uio.h>

struct opio;

struct opio_list {
	struct opio        *head;
	struct opio        *tail;
};

#define UIO_FASTIOV 8

struct opio {
	struct iovec       iov[UIO_FASTIOV];
	struct iocb        orig_iocb;
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

static inline size_t
iocb_nbytes(const struct iocb* io)
{
	switch(io->aio_lio_opcode) {
		case IO_CMD_PREAD: /* fall-through */
		case IO_CMD_PWRITE:
			return io->u.c.nbytes;

		case IO_CMD_FSYNC:
		case IO_CMD_FDSYNC: /* fall-through */
		case IO_CMD_NOOP: /* fall-through */
			return 0;

		case IO_CMD_POLL:
			/* result is not an amount */
			ASSERT(0);

		case IO_CMD_PREADV: /* fall-through */
		case IO_CMD_PWRITEV:
			{
				size_t sum = 0, i;
				for(i=0; i<io->u.v.nr; i++) {
					ASSERT(io->u.v.vec[i].iov_len > 0);
					sum += io->u.v.vec[i].iov_len;
				}
				return sum;
			}
		default:
			ASSERT(0);
	}
}

static inline long long
iocb_offset(const struct iocb* io)
{
	switch(io->aio_lio_opcode) {
		case IO_CMD_PREAD: /* fall-through */
		case IO_CMD_PWRITE:
			return io->u.c.offset;

		case IO_CMD_PREADV: /* fall-through */
		case IO_CMD_PWRITEV:
			return io->u.v.offset;

		default:
			/* no offset in other commands */
			ASSERT(0);
	}
}

static inline void* iocb_buf(const struct iocb *io)
{
	ASSERT(io->aio_lio_opcode == IO_CMD_PREAD
			|| io->aio_lio_opcode == IO_CMD_PWRITE);
	return io->u.c.buf;
}

static inline const char* iocb_opcode(const struct iocb* io)
{
	switch(io->aio_lio_opcode) {
		case IO_CMD_PREAD:
			return "read";
		case IO_CMD_PWRITE:
			return "write";
		case IO_CMD_FSYNC:
			return "fsync";
		case IO_CMD_FDSYNC:
			return "fdsync";
		case IO_CMD_POLL:
			return "poll";
		case IO_CMD_NOOP:
			return "noop";
		case IO_CMD_PREADV:
			return "preadv";
		case IO_CMD_PWRITEV:
			return "pwritev";
		default:
			ASSERT(0);
	}
}

static inline int iocb_vectorized(io_iocb_cmd_t op)
{
	switch (op) {
		case IO_CMD_PREAD:
			return IO_CMD_PREADV;
		case IO_CMD_PWRITE:
			return IO_CMD_PWRITEV;
		default:
			return op;
	}
}

#endif
