/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 */

#ifndef TAPDISK_QUEUE_H
#define TAPDISK_QUEUE_H

#include <libaio.h>

#include "io-optimize.h"

struct tlog;
struct tfilter;
struct disk_driver;

struct tiocb {
	void                 *data;
	struct iocb           iocb;
	struct tiocb         *next;
	struct disk_driver   *dd;
};

struct tlist {
	struct tiocb         *head;
	struct tiocb         *tail;
};

struct tqueue {
	int                   size;
	int                   sync;

	int                   poll_fd;
	io_context_t          aio_ctx;
	struct opioctx        opioctx;
	int                   dummy_pipe[2];

	int                   queued;
	int                   pending;
	struct iocb         **iocbs;
	struct io_event      *aio_events;

	/* iocbs may be deferred if the aio ring is full.
	 * tapdisk_queue_complete will ensure deferred
	 * iocbs are queued as slots become available. */
	struct tlist          deferred;

	/* optional tapdisk filter */
	struct tfilter       *filter;

	/* optional log handle */
	struct tlog          *log;
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
#define tapdisk_queue_count(q) ((q)->queued)
#define tapdisk_queue_empty(q) ((q)->queued == 0)
#define tapdisk_queue_full(q)  (((q)->pending + (q)->queued) >= (q)->size)
int tapdisk_init_queue(struct tqueue *, int size, int sync,
		       struct tlog *, struct tfilter *);
void tapdisk_free_queue(struct tqueue *);
void tapdisk_debug_queue(struct tqueue *);
void tapdisk_queue_tiocb(struct tqueue *, struct tiocb *);
int tapdisk_submit_tiocbs(struct tqueue *);
int tapdisk_submit_all_tiocbs(struct tqueue *);
int tapdisk_complete_tiocbs(struct tqueue *);
int tapdisk_cancel_tiocbs(struct tqueue *);
int tapdisk_cancel_all_tiocbs(struct tqueue *);
void tapdisk_prep_tiocb(struct tiocb *, struct disk_driver *, int fd, int rw,
			char *buf, size_t size, long long offset, void *data);

/*
 * Convenience wrappers for request consumers (i.e., plugins)
 * NB: dd->td_state->queue must be properly initialized by request producer
 */
void td_queue_tiocb(struct disk_driver *dd, struct tiocb *tiocb);
void td_prep_read(struct tiocb *tiocb, struct disk_driver *dd, int fd,
		  char *buf, size_t bytes, long long offset, void *data);
void td_prep_write(struct tiocb *tiocb, struct disk_driver *dd, int fd,
		   char *buf, size_t bytes, long long offset, void *data);

#endif
