/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 */

#ifndef TAPDISK_QUEUE_H
#define TAPDISK_QUEUE_H

#include <libaio.h>
#include <pthread.h>

#include "io-optimize.h"
#include "scheduler.h"

struct tiocb;
struct tfilter;

typedef void (*td_queue_callback_t)(void *arg, struct tiocb *, int err);

struct tiocb {
	td_queue_callback_t   cb;
	void                 *arg;

	struct iocb           iocb;
	struct list_head      entry;
};

struct tqueue {
	int                   size;

	const struct tio     *tio;
	void                 *tio_data;

	struct opioctx        opioctx;

	struct iocb         **iocbs;
	int                   queued;

	struct list_head      waiting; /* tiocbs deferred */
	struct list_head      pending; /* tiocbs submitted */

	/* iocbs <= tiocbs pending, due to coalescing */
	int                   iocbs_pending;
	int                   tiocbs_pending;

	/* optional tapdisk filter */
	struct tfilter       *filter;

	/* tio_submit thread */
	pthread_t             thread;
	pthread_cond_t        cond;
	pthread_mutex_t       mutex;
	int                   closing;
};

struct tio {
	const char           *name;
	size_t                data_size;

	int  (*tio_setup)    (struct tqueue *queue, int qlen);
	void (*tio_destroy)  (struct tqueue *queue);
	int  (*tio_submit)   (struct tqueue *queue);
};

enum {
	TIO_DRV_LIO     = 1,
	TIO_DRV_RWIO    = 2,
};

/*
 * Interface for request producer (i.e., tapdisk)
 * NB: the following functions may cause additional tiocbs to be queued:
 *        - tapdisk_submit_tiocbs
 *        - tapdisk_cancel_tiocbs
 *        - tapdisk_complete_tiocbs
 * The *_all_tiocbs variants will handle the first two cases;
 * be sure to call submit after calling complete in the third case.
 */
int tapdisk_init_queue(struct tqueue *, int size, int drv, struct tfilter *);
void tapdisk_free_queue(struct tqueue *);
void tapdisk_debug_queue(struct tqueue *);
void tapdisk_queue_tiocb(struct tqueue *, struct tiocb *);
int tapdisk_submit_tiocbs(struct tqueue *);
void tapdisk_submit_all_tiocbs(struct tqueue *);
int tapdisk_cancel_tiocbs(struct tqueue *);
int tapdisk_cancel_all_tiocbs(struct tqueue *);
void tapdisk_prep_tiocb(struct tiocb *, int, int, char *, size_t,
			long long, td_queue_callback_t, void *);

#endif
