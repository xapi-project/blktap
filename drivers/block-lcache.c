/*
 * Copyright (c) 2010, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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

/* Local persistent cache: write any sectors not found in the leaf back to the 
 * leaf.
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/vfs.h>

#include "vhd.h"
#include "tapdisk.h"
#include "tapdisk-utils.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-interface.h"

#define DEBUG 1

#ifdef DEBUG
#define DBG(_f, _a...) tlog_write(TLOG_DBG, _f, ##_a)
#else
#define DBG(_f, _a...) ((void)0)
#endif
#define WARN(_f, _a...) tlog_syslog(TLOG_WARN, "WARNING: "_f "in %s:%d", \
				    ##_a, __func__, __LINE__)
#define INFO(_f, _a...) tlog_syslog(TLOG_INFO, _f, ##_a)
#define BUG()           td_panic()
#define BUG_ON(_cond)   if (unlikely(_cond)) { td_panic(); }
#define WARN_ON(_p)     if (unlikely(_cond)) { WARN(_cond); }

#define MIN(a, b)       ((a) < (b) ? (a) : (b))

#define TD_LCACHE_MAX_REQ               (MAX_REQUESTS*2)
#define TD_LCACHE_BUFSZ                 (MAX_SEGMENTS_PER_REQ * \
					 sysconf(_SC_PAGE_SIZE))


typedef struct lcache                   td_lcache_t;
typedef struct lcache_request           td_lcache_req_t;

struct lcache_request {
	char                           *buf;
	int                             err;

	td_request_t                    treq;
	int                             secs;

	td_vbd_request_t                vreq;
	struct td_iovec                 iov;

	td_lcache_t                    *cache;
};

struct lcache {
	char                           *name;

	td_lcache_req_t                 reqv[TD_LCACHE_MAX_REQ];
	td_lcache_req_t                *free[TD_LCACHE_MAX_REQ];
	int                             n_free;

	char                           *buf;
	size_t                          bufsz;

	int                             wr_en;
	struct timeval                  ts;
};

static td_lcache_req_t *
lcache_alloc_request(td_lcache_t *cache)
{
	td_lcache_req_t *req = NULL;

	if (likely(cache->n_free))
		req = cache->free[--cache->n_free];

	return req;
}

static void
lcache_free_request(td_lcache_t *cache, td_lcache_req_t *req)
{
	BUG_ON(cache->n_free >= TD_LCACHE_MAX_REQ);
	cache->free[cache->n_free++] = req;
}

static void
lcache_destroy_buffers(td_lcache_t *cache)
{
	td_lcache_req_t *req;

	do {
		req = lcache_alloc_request(cache);
		if (req)
			munmap(req->buf, TD_LCACHE_BUFSZ);
	} while (req);
}

static int
lcache_create_buffers(td_lcache_t *cache)
{
	int prot, flags, i, err;
	size_t bufsz;

	prot  = PROT_READ|PROT_WRITE;
	flags = MAP_ANONYMOUS|MAP_PRIVATE|MAP_LOCKED;

	cache->n_free = 0;

	for (i = 0; i < TD_LCACHE_MAX_REQ; i++) {
		td_lcache_req_t *req = &cache->reqv[i];

		req->buf = mmap(NULL, TD_LCACHE_BUFSZ, prot, flags, -1, 0);
		if (req->buf == MAP_FAILED) {
			req->buf = NULL;
			err = -errno;
			goto fail;
		}

		lcache_free_request(cache, req);
	}

	return 0;

fail:
	EPRINTF("Buffer init failure: %d", err);
	lcache_destroy_buffers(cache);
	return err;
}

static int
lcache_close(td_driver_t *driver)
{
	td_lcache_t *cache = driver->data;

	lcache_destroy_buffers(cache);

	free(cache->name);

	return 0;
}

static int
lcache_open(td_driver_t *driver, const char *name, td_flag_t flags)
{
	td_lcache_t *cache = driver->data;
	int i, err;
	int prot, _flags;
	size_t lreq_bufsz;

	err  = tapdisk_namedup(&cache->name, (char *)name);
	if (err)
		goto fail;

	err = lcache_create_buffers(cache);
	if (err)
		goto fail;

	timerclear(&cache->ts);
	cache->wr_en = 1;

	return 0;

fail:
	lcache_close(driver);
	return err;
}

/*
 * NB. lcache->{wr_en,ts}: test free space in the caching SR before
 * attempting to store our reads. VHD block allocation writes on Ext3
 * have the nasty property of blocking excessively after running out
 * of space. We therefore enable/disable ourselves at a 1/s
 * granularity, querying free space through statfs beforehand.
 */

static long
lcache_fs_bfree(const td_lcache_t *cache, long *bsize)
{
	struct statfs fst;
	int err;

	err = statfs(cache->name, &fst);
	if (err)
		return err;

	if (likely(bsize))
		*bsize = fst.f_bsize;

	return MIN(fst.f_bfree, LONG_MAX);
}

static int
__lcache_wr_enabled(const td_lcache_t *cache)
{
	long threshold = 2<<20; /* B */
	long bfree, bsz = 1;
	int enable;

	bfree  = lcache_fs_bfree(cache, &bsz);
	enable = bfree > threshold / bsz;

	return enable;
}

static int
lcache_wr_enabled(td_lcache_t *cache)
{
	const int timeout = 1; /* s */
	struct timeval now, delta;

	gettimeofday(&now, NULL);
	timersub(&now, &cache->ts, &delta);

	if (delta.tv_sec >= timeout) {
		cache->wr_en = __lcache_wr_enabled(cache);
		cache->ts    = now;
	}

	return cache->wr_en;
}

static void
__lcache_write_cb(td_vbd_request_t *vreq, int error,
		  void *token, int final)
{
	td_lcache_req_t *req = containerof(vreq, td_lcache_req_t, vreq);
	td_lcache_t *cache = token;

	if (error == -ENOSPC)
		cache->wr_en = 0;

	lcache_free_request(cache, req);
}

static void
lcache_store_read(td_lcache_t *cache, td_lcache_req_t *req)
{
	td_vbd_request_t *vreq;
	struct td_iovec *iov;
	td_vbd_t *vbd;
	int err;

	iov          = &req->iov;
	iov->base    = req->buf;
	iov->secs    = req->treq.secs;

	vreq         = &req->vreq;
	vreq->op     = TD_OP_WRITE;
	vreq->sec    = req->treq.sec;
	vreq->iov    = iov;
	vreq->iovcnt = 1;
	vreq->cb     = __lcache_write_cb;
	vreq->token  = cache;

	vbd = req->treq.vreq->vbd;

	err = tapdisk_vbd_queue_request(vbd, vreq);
	BUG_ON(err);
}

static void
lcache_complete_read(td_lcache_t *cache, td_lcache_req_t *req)
{
	td_vbd_request_t *vreq;

	vreq = req->treq.vreq;

	if (likely(!req->err)) {
		size_t sz = req->treq.secs << SECTOR_SHIFT;
		memcpy(req->treq.buf, req->buf, sz);
	}

	td_complete_request(req->treq, req->err);

	if (unlikely(req->err) || !lcache_wr_enabled(cache)) {
		lcache_free_request(cache, req);
		return;
	}

	lcache_store_read(cache, req);
}

static void
__lcache_read_cb(td_request_t treq, int err)
{
	td_lcache_req_t *req = treq.cb_data;
	td_lcache_t *cache = req->cache;

	BUG_ON(req->secs < treq.secs);
	req->secs -= treq.secs;
	req->err   = req->err ? : err;

	if (!req->secs)
		lcache_complete_read(cache, req);
}

static void
lcache_queue_read(td_driver_t *driver, td_request_t treq)
{
	td_lcache_t *cache = driver->data;
	int err;
	size_t size;
	td_request_t clone;
	td_lcache_req_t *req;

	req = lcache_alloc_request(cache);
	if (!req) {
		td_complete_request(treq, -EBUSY);
		return;
	}

	req->treq    = treq;
	req->cache   = cache;

	req->secs    = req->treq.secs;
	req->err     = 0;

	clone         = treq;
	clone.buf     = req->buf;
	clone.cb      = __lcache_read_cb;
	clone.cb_data = req;

out:
	td_forward_request(clone);
}

static int
lcache_get_parent_id(td_driver_t *driver, td_disk_id_t *id)
{
	return -EINVAL;
}

static int
lcache_validate_parent(td_driver_t *driver,
		       td_driver_t *pdriver, td_flag_t flags)
{
	if (strcmp(driver->name, pdriver->name))
		return -EINVAL;

	return 0;
}

struct tap_disk tapdisk_lcache = {
	.disk_type                  = "tapdisk_lcache",
	.flags                      = 0,
	.private_data_size          = sizeof(td_lcache_t),
	.td_open                    = lcache_open,
	.td_close                   = lcache_close,
	.td_queue_read              = lcache_queue_read,
	.td_get_parent_id           = lcache_get_parent_id,
	.td_validate_parent         = lcache_validate_parent,
};
