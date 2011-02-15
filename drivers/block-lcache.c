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
#include <sys/mman.h>

#include "vhd.h"
#include "tapdisk.h"
#include "tapdisk-utils.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-interface.h"

#ifdef DEBUG
#define DBG(_f, _a...) tlog_write(TLOG_DBG, _f, ##_a)
#else
#define DBG(_f, _a...) ((void)0)
#endif

#define WARN(_f, _a...) tlog_write(TLOG_WARN, _f, ##_a)
#define BUG()           td_panic()
#define BUG_ON(_cond)   if (_cond) { td_panic(); }

#define TD_LCACHE_MAX_REQ               (MAX_REQUESTS*2)
#define TD_LCACHE_BUFSZ                 (MAX_SEGMENTS_PER_REQ * \
					 sysconf(_SC_PAGE_SIZE))


typedef struct lcache                   td_lcache_t;
typedef struct lcache_request           td_lcache_req_t;

struct lcache_request {
	int                             err;
	char                           *buf;
	int                             secs;
	td_request_t                    treq;
	td_lcache_t                    *cache;
	enum { LC_READ = 1, LC_WRITE }  phase;
};

struct lcache {
	char                           *name;

	td_lcache_req_t                 reqv[TD_LCACHE_MAX_REQ];
	td_lcache_req_t                *free[TD_LCACHE_MAX_REQ];
	int                             n_free;

	char                           *buf;
	size_t                          bufsz;
};

static void lcache_complete_req(td_request_t, int);

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

static int
lcache_close(td_driver_t *driver)
{
	td_lcache_t *cache = driver->data;

	DPRINTF("Closing local cache for %s\n", cache->name);

	if (cache->buf)
		munmap(cache->buf, cache->bufsz);

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

	err   = tapdisk_namedup(&cache->name, (char *)name);
	if (err)
		goto fail;

	lreq_bufsz   = MAX_SEGMENTS_PER_REQ * sysconf(_SC_PAGE_SIZE);
	cache->bufsz = TD_LCACHE_MAX_REQ * lreq_bufsz;

	prot   = PROT_READ|PROT_WRITE;
	_flags = MAP_ANONYMOUS|MAP_PRIVATE;
	cache->buf = mmap(NULL, cache->bufsz, prot, _flags, -1, 0);
	if (cache->buf == MAP_FAILED) {
		cache->buf == NULL;
		err = -errno;
		goto fail;
	}

	err = mlock(cache->buf, cache->bufsz);
	if (err) {
		err = -errno;
		goto fail;
	}

	cache->n_free = TD_LCACHE_MAX_REQ;
	for (i = 0; i < TD_LCACHE_MAX_REQ; i++) {
		td_lcache_req_t *req = &cache->reqv[i];
		req->buf = cache->buf + i * lreq_bufsz;
		cache->free[i] = req;
	}

	DPRINTF("Opening local cache for %s\n", cache->name);
	return 0;

fail:
	lcache_close(driver);
	return err;
}

static void
lcache_complete_read(td_lcache_t *cache, td_lcache_req_t *req)
{
	td_vbd_request_t *vreq;
	td_vbd_t *vbd;
	td_request_t clone;

	vreq = req->treq.vreq;
	vbd  = vreq->vbd;

	if (!req->err) {
		size_t sz = req->treq.secs << SECTOR_SHIFT;
		memcpy(req->treq.buf, req->buf, sz);
	}

	td_complete_request(req->treq, req->err);

	if (req->err) {
		lcache_free_request(cache, req);
		return;
	}

	req->phase   = LC_WRITE;
	req->secs    = req->treq.secs;
	req->err     = 0;

	clone         = req->treq;
	clone.op      = TD_OP_WRITE;
	clone.buf     = req->buf;
	clone.cb      = lcache_complete_req;
	clone.cb_data = req;
	clone.image   = tapdisk_vbd_first_image(vbd);

	td_queue_write(clone.image, clone);
}

static void
lcache_complete_write(td_lcache_t *cache, td_lcache_req_t *req)
{
	lcache_free_request(cache, req);
}

static void
lcache_complete_req(td_request_t treq, int err)
{
	td_lcache_req_t *req = treq.cb_data;
	td_lcache_t *cache = req->cache;

	BUG_ON(req->secs == 0);
	BUG_ON(req->secs < treq.secs);

	req->secs -= treq.secs;
	req->err   = req->err ? : err;

	if (req->secs)
		return;

	switch (req->phase) {
	case LC_READ:
		lcache_complete_read(cache, req);
		break;

	case LC_WRITE:
		lcache_complete_write(cache, req);
		break;

	default:
		BUG();
	}
}

static void
lcache_queue_read(td_driver_t *driver, td_request_t treq)
{
	td_lcache_t *cache = driver->data;
	int err;
	size_t size;
	td_request_t clone;
	td_lcache_req_t *req;

	//DPRINTF("LocalCache: read request! %lld (%d secs)\n", treq.sec, 
	//treq.secs);

	req = lcache_alloc_request(cache);
	if (!req) {
		td_forward_request(treq);
		return;
	}

	req->treq    = treq;
	req->cache   = cache;

	req->phase   = LC_READ;
	req->secs    = req->treq.secs;
	req->err     = 0;

	clone         = treq;
	clone.buf     = req->buf;
	clone.cb      = lcache_complete_req;
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

static void
lcache_debug(td_driver_t *driver)
{
	td_lcache_t *cache = driver->data;

	WARN("LOCAL CACHE %s\n", cache->name);
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
	.td_debug                   = lcache_debug,
};
