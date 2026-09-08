/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef IO_BACKEND_H
#define IO_BACKEND_H

#include <aio.h>
#include <libaio.h>

struct tiocb;
struct tfilter;
typedef void* tqueue;
typedef void (*td_queue_callback_t)(void *arg, struct tiocb *, int err);

union uioc {
	struct aiocb aio;
	struct iocb io;
};

struct tiocb {
	td_queue_callback_t   cb;
	void                 *arg;

        union uioc	      uiocb;
	struct tiocb         *next;
};

struct tlist {
	struct tiocb         *head;
	struct tiocb         *tail;
};


typedef void (*debug_queue)(tqueue );
typedef int (*init_queue)(tqueue* , int size, int drv, struct tfilter *);
typedef	void (*free_queue)(tqueue* );
typedef	void (*up_queue)(tqueue , struct tiocb *);
typedef	int  (*submit_all_queue)(tqueue );
typedef	int (*submit_tiocbs_queue)(tqueue );
typedef	void (*prep_tiocb_queue)(struct tiocb *, int, int, char *, size_t,
			long long, td_queue_callback_t, void *);

struct backend {
	debug_queue debug;
	init_queue init;
	free_queue free_queue;
	up_queue queue;
	submit_all_queue submit_all;
	submit_tiocbs_queue submit_tiocbs;
	prep_tiocb_queue prep;
};

#endif /*IO_BACKEND_H*/
