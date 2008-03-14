/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 */

#ifndef TAPDISK_FILTER_H
#define TAPDISK_FILTER_H

#include <libaio.h>
#include <inttypes.h>
#include <time.h>

#define TD_INJECT_FAULTS     0x00001  /* simulate random IO failures */
#define TD_CHECK_INTEGRITY   0x00002  /* check data integrity */

#define TD_FAULT_RATE        5

struct dhash {
	uint64_t             hash;
	struct timeval       time;
};

struct fiocb {
	size_t               bytes;
	void                *data;
};

struct tfilter {
	int                  mode;
	uint64_t             secs;
	int                  iocbs;

	struct dhash        *dhash;

	int                  ffree;
	struct fiocb        *fiocbs;
	struct fiocb       **flist;
};

struct tfilter *tapdisk_init_tfilter(int mode, int iocbs, uint64_t secs);
void tapdisk_free_tfilter(struct tfilter *);
void tapdisk_filter_iocbs(struct tfilter *, struct iocb **, int);
void tapdisk_filter_events(struct tfilter *, struct io_event *, int);

#endif
