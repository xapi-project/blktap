/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Driver to sit on top of another disk and log writes, in order
 * to track changed blocks
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "tapdisk.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-utils.h"
#include "timeout-math.h"
#include "log.h"

#define MAX_CONNECTIONS 1

typedef struct poll_fd {
	int          fd;
	event_id_t   id;
} poll_fd_t;

struct tdlog_state {
	uint64_t     size;
  	void*        writelog;
};

/* -- write log -- */

/* large flat bitmaps don't scale particularly well either in size or scan
 * time, but they'll do for now */
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(bits) (((bits)+BITS_PER_LONG-1)/BITS_PER_LONG)

#define BITMAP_ENTRY(_nr, _bmap) ((unsigned long*)(_bmap))[(_nr)/BITS_PER_LONG]
#define BITMAP_SHIFT(_nr) ((_nr) % BITS_PER_LONG)

static inline int test_bit(int nr, void* bmap)
{
	return (BITMAP_ENTRY(nr, bmap) >> BITMAP_SHIFT(nr)) & 1;
}

static inline void clear_bit(int nr, void* bmap)
{
	BITMAP_ENTRY(nr, bmap) &= ~(1UL << BITMAP_SHIFT(nr));
}

static inline void set_bit(int nr, void* bmap)
{
	BITMAP_ENTRY(nr, bmap) |= (1UL << BITMAP_SHIFT(nr));
}

static inline int bitmap_size(uint64_t sz)
{
	return sz >> 3;
}

/* if end is 0, clear to end of disk */
int writelog_clear(struct tdlog_state* s, uint64_t start, uint64_t end)
{
	if (!end)
		end = s->size;

	/* clear to word boundaries */
	while (BITMAP_SHIFT(start))
		clear_bit(start++, s->writelog);
	while (BITMAP_SHIFT(end))
		clear_bit(end--, s->writelog);

	memset(s->writelog + start / BITS_PER_LONG, 0, (end - start) >> 3);

	return 0;
}

static int writelog_create(struct tdlog_state *s)
{
	uint64_t bmsize;

	bmsize = bitmap_size(s->size);

	DPRINTF("allocating %"PRIu64" bytes for dirty bitmap", bmsize);

	if (!(s->writelog = calloc(bmsize, 1))) {
		EPRINTF("could not allocate dirty bitmap of size %"PRIu64, bmsize);
		return -1;
	}

	return 0;
}

static int writelog_free(struct tdlog_state *s)
{
	if (s->writelog)
		free(s->writelog);

	return 0;
}

static int writelog_set(struct tdlog_state* s, uint64_t sector, int count)
{
	int i;
	EPRINTF("Setting %d bits starting at sector %"PRIu64"\n", count, sector);

	for (i = 0; i < count; i++) 
		set_bit(sector + i, s->writelog);

	return 0;
}


/* -- interface -- */

static int tdlog_close(td_driver_t* driver)
{
	struct tdlog_state* s = (struct tdlog_state*)driver->data;
	writelog_free(s);

	return 0;
}

static int tdlog_open(td_driver_t* driver, const char* name, td_flag_t flags)
{
	struct tdlog_state* s = (struct tdlog_state*)driver->data;
	int rc;

	memset(s, 0, sizeof(*s));

	s->size = driver->info.size;

	DPRINTF("Size of original image is %"PRIu64"\n", s->size);
	if ((rc = writelog_create(s))) {
		tdlog_close(driver);
		return rc;
	}

	return 0;
}

static void tdlog_queue_read(td_driver_t* driver, td_request_t treq)
{
	td_forward_request(treq);
}

static void tdlog_queue_write(td_driver_t* driver, td_request_t treq)
{
	struct tdlog_state* s = (struct tdlog_state*)driver->data;

	writelog_set(s, treq.sec, treq.secs);
	td_forward_request(treq);
}

static int tdlog_get_parent_id(td_driver_t* driver, td_disk_id_t* id)
{
	return -EINVAL;
}

static int tdlog_validate_parent(td_driver_t *driver,
				 td_driver_t *parent, td_flag_t flags)
{
	return 0;
}

struct tap_disk tapdisk_log = {
	.disk_type          = "tapdisk_log",
	.private_data_size  = sizeof(struct tdlog_state),
	.flags              = 0,
	.td_open            = tdlog_open,
	.td_close           = tdlog_close,
	.td_queue_read      = tdlog_queue_read,
	.td_queue_write     = tdlog_queue_write,
	.td_get_parent_id   = tdlog_get_parent_id,
	.td_validate_parent = tdlog_validate_parent,
};
