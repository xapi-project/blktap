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

#include <errno.h>

#include "tapdisk.h"
#include "tapdisk-vbd.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-disktype.h"

#define DBG(_f, _a...)  tlog_syslog(TLOG_DBG, _f, ##_a)
#define INFO(_f, _a...) tlog_syslog(TLOG_INFO, _f, ##_a)
#define WARN(_f, _a...) tlog_syslog(TLOG_WARN, "WARNING: "_f "in %s:%d", \
				    ##_a, __func__, __LINE__)

#define BUG()           td_panic()
#define BUG_ON(_cond)   if (unlikely(_cond)) { td_panic(); }
#define WARN_ON(_p)     if (unlikely(_cond)) { WARN(_cond); }

int ll_write_error(int curr, int error)
{
	if (error && (!curr || curr == -ENOSPC))
		return error;

	return 0;
}

void ll_log_switch(int type, int error,
		   td_image_t *local, td_image_t *shared)
{
	WARN("WARNING: %s, on %s:%s. Switching to %s:%s.",
	     strerror(-error),
	     tapdisk_disk_types[local->type]->name, local->name,
	     tapdisk_disk_types[shared->type]->name, shared->name);
}

/*
 * LLP: Local leaf persistent cache
 *      -- Persistent write caching in local storage.
 *
 *    VBD
 *      \
 *       +--r/w--> llp+vhd:/local/leaf
 *        \
 *         +--r/w--> vhd:/shared/leaf
 *          \
 *           +--r/o--> vhd:/shared/parent
 *
 * We drive two 'leaf' (r/w) images: One LOCAL (i.e. on local storage,
 * unreliable and prone to out-of-space failures), and one SHARED
 * (i.e. in shared storage with plenty of physical backing).
 *
 * All images are on a linear read chain: LOCAL inherits from SHARED,
 * which inherits from a shared master image. This filter driver
 * aggregates LOCAL. SHARED is our immediate parent, forced into R/W
 * mode.
 *
 * Unless LOCAL failed, reads are issued to LOCAL, to save shared
 * storage bandwidth. In case of failure, SHARED provides continued
 * VDI consistency.
 *
 */
enum {
	LLP_MIRROR = 1,
	/*
	 * LLP_MIRROR:
	 *
	 * Writes are mirrored to both LOCAL and SHARED. Reads are
	 * issued to LOCAL.
	 *
	 * Failure to write LOCAL are recoverable. The driver will
	 * transition to LLP_SHARED.
	 *
	 * Failure to write SHARED is irrecoverable, and signaled to
	 * the original issuer.
	 */

	LLP_SHARED = 2,
	/*
	 * LLP_SHARED:
	 *
	 * Writes are issued to SHARED only. As are reads.
	 *
	 * Failure to write SHARED is irrecoverable.
	 */
};

typedef struct llpcache                 td_llpcache_t;
typedef struct llpcache_request         td_llpcache_req_t;
#define TD_LLPCACHE_MAX_REQ             (MAX_REQUESTS*2)

struct llpcache_vreq {
	enum { LOCAL = 0, SHARED = 1 }  target;
	td_vbd_request_t                vreq;
};

struct llpcache_request {
	td_request_t            treq;

	struct td_iovec         iov;
	int                     error;

	struct llpcache_vreq    lvr[2];

	unsigned int            pending;
	int                     mode;
};

struct llpcache {
	td_image_t             *local;
	int                     mode;

	td_llpcache_req_t       reqv[TD_LLPCACHE_MAX_REQ];
	td_llpcache_req_t      *free[TD_LLPCACHE_MAX_REQ];
	int                     n_free;
};

static void
llpcache_close_image(td_llpcache_t *s)
{
}

static td_llpcache_req_t *
llpcache_alloc_request(td_llpcache_t *s)
{
	td_llpcache_req_t *req = NULL;

	if (likely(s->n_free))
		req = s->free[--s->n_free];

	return req;
}

static void
llpcache_free_request(td_llpcache_t *s, td_llpcache_req_t *req)
{
	BUG_ON(s->n_free >= TD_LLPCACHE_MAX_REQ);
	s->free[s->n_free++] = req;
}

static void
__llpcache_write_cb(td_vbd_request_t *vreq, int error,
		   void *token, int final)
{
	td_llpcache_t *s = token;
	struct llpcache_vreq *lvr;
	td_llpcache_req_t *req;
	int mask;

	lvr = containerof(vreq, struct llpcache_vreq, vreq);
	req = containerof(lvr, td_llpcache_req_t, lvr[lvr->target]);

	mask = 1U << lvr->target;
	BUG_ON(!(req->pending & mask))

	if (lvr->target == LOCAL && error == -ENOSPC) {
		td_image_t *shared =
			containerof(req->treq.image->next.next,
				    td_image_t, next);
		ll_log_switch(DISK_TYPE_LLPCACHE, error,
			      s->local, shared);
		s->mode = LLP_SHARED;
		error = 0;
	}

	req->pending &= ~mask;
	req->error    = ll_write_error(req->error, error);

	if (!req->pending) {
		/* FIXME: Make sure this won't retry. */
		td_complete_request(req->treq, req->error);
		llpcache_free_request(s, req);
	}
}

/*
 * NB. Write mirroring. Lacking per-image queues, it's still a
 * hack. But shall do for now:
 *
 *   1. Store the treq, thereby blocking the original vreq.
 *   2. Reissue, as two clone vreqs. One local, one shared.
 *   3. Clones seen again then get forwarded.
 *   4. Treq completes after both vreqs.
 *
 * We can recognize clones by matching the vreq->token field.
 */

static int
llpcache_requeue_treq(td_llpcache_t *s, td_llpcache_req_t *req, int target)
{
	struct llpcache_vreq *lvr;
	td_vbd_request_t *vreq;
	td_vbd_t *vbd;
	int err;

	lvr           = &req->lvr[target];
	lvr->target   = target;

	vreq          = &lvr->vreq;
	vreq->op      = TD_OP_WRITE;
	vreq->sec     = req->treq.sec;
	vreq->iov     = &req->iov;
	vreq->iovcnt  = 1;
	vreq->cb      = __llpcache_write_cb;
	vreq->token   = s;

	err = tapdisk_vbd_queue_request(req->treq.vreq->vbd, vreq);
	if (err)
		goto fail;

	req->pending |= 1UL << target;
	return 0;

fail:
	req->error   = req->error ? : err;
	return err;
}

static void
llpcache_fork_write(td_llpcache_t *s, td_request_t treq)
{
	td_llpcache_req_t *req;
	struct td_iovec *iov;
	int err;

	req = llpcache_alloc_request(s);
	if (!req) {
		td_complete_request(treq, -EBUSY);
		return;
	}

	memset(req, 0, sizeof(req));

	req->treq     = treq;

	iov           = &req->iov;
	iov->base     = treq.buf;
	iov->secs     = treq.secs;

	err = llpcache_requeue_treq(s, req, LOCAL);
	if (err)
		goto fail;

	err = llpcache_requeue_treq(s, req, SHARED);
	if (err)
		goto fail;

	return;

fail:
	if (!req->pending) {
		td_complete_request(treq, req->error);
		llpcache_free_request(s, req);
	}
}

static void
llpcache_forward_write(td_llpcache_t *s, td_request_t treq)
{
	const td_vbd_request_t *vreq = treq.vreq;
	struct llpcache_vreq *lvr;

	lvr = containerof(vreq, struct llpcache_vreq, vreq);

	switch (lvr->target) {
	case SHARED:
		td_forward_request(treq);
		break;
	case LOCAL:
		td_queue_write(s->local, treq);
		break;
	default:
		BUG();
	}
}

static void
llpcache_queue_write(td_driver_t *driver, td_request_t treq)
{
	td_llpcache_t *s = driver->data;

	if (treq.vreq->token == s)
		llpcache_forward_write(s, treq);
	else
		llpcache_fork_write(s, treq);
}

static void
llpcache_queue_read(td_driver_t *driver, td_request_t treq)
{
	td_llpcache_t *s = driver->data;

	switch (s->mode) {
	case LLP_MIRROR:
		td_queue_read(s->local, treq);
		break;
	case LLP_SHARED:
		td_forward_request(treq);
	default:
		BUG();
	}
}

static int
llpcache_close(td_driver_t *driver)
{
	td_llpcache_t *s = driver->data;

	if (s->local) {
		tapdisk_image_close(s->local);
		s->local = NULL;
	}

	return 0;
}

static int
llpcache_open(td_driver_t *driver, const char *name, td_flag_t flags)
{
	td_llpcache_t *s = driver->data;
	int i, err;

	s->mode = LLP_MIRROR;

	for (i = 0; i < TD_LLPCACHE_MAX_REQ; i++)
		llpcache_free_request(s, &s->reqv[i]);

	err = tapdisk_image_open(DISK_TYPE_VHD, name, flags, &s->local);
	if (err)
		goto fail;

	driver->info = s->local->driver->info;

	return 0;

fail:
	llpcache_close(driver);
	return err;
}

static int
llcache_get_parent_id(td_driver_t *driver, td_disk_id_t *id)
{
	td_llpcache_t *s = driver->data;
	int err;

	err = td_get_parent_id(s->local, id);
	if (!err)
		id->flags &= ~TD_OPEN_RDONLY;

	return err;
}

static int
llcache_validate_parent(td_driver_t *driver,
			td_driver_t *pdriver, td_flag_t flags)
{
	return -ENOSYS;
}


struct tap_disk tapdisk_llpcache = {
	.disk_type                  = "tapdisk_llpcache",
	.flags                      = 0,
	.private_data_size          = sizeof(td_llpcache_t),
	.td_open                    = llpcache_open,
	.td_close                   = llpcache_close,
	.td_queue_read              = llpcache_queue_read,
	.td_queue_write             = llpcache_queue_write,
	.td_get_parent_id           = llcache_get_parent_id,
	.td_validate_parent         = llcache_validate_parent,
};

/*
 * LLE: Local Leaf Ephemeral Cache
 *      -- Non-persistent write caching in local storage.
 *
 *    VBD
 *      \
 *       +--r/w--> lle+vhd:/shared/leaf
 *        \
 *         +--r/w--> vhd:/local/leaf
 *          \
 *           +--r/o--> vhd:/shared/parent
 *
 * Note that LOCAL and SHARED chain order differs from LLP. Shared
 * storage data masks local data.
 *
 * This means VDI state in shared storage state alone is
 * inconsistent. Wherever local is unavailable, SHARED must be
 * discarded too.
 */
enum {
	LLE_LOCAL = 1,
	/*
	 * LLE_LOCAL:
	 *
	 * Writes are forwarded to LOCAL only. As are reads. This
	 * reduces network overhead.
	 *
	 * Failure to write LOCAL is recoverable. The driver will
	 * transition to LLE_SHARED.
	 *
	 * Failure to write to shared are irrecoverable and signaled
	 * to the original issuer.
	 */

	LLE_SHARED = 2,
	/*
	 * LLE_SHARED:
	 *
	 * Writes are issued to SHARED. As are reads.
	 *
	 * Failure to write to SHARED is irrecoverable.
	 */
};

typedef struct llecache                 td_llecache_t;
typedef struct llecache_request         td_llecache_req_t;
#define TD_LLECACHE_MAX_REQ             (MAX_REQUESTS*2)

struct llecache_request {
	td_llecache_t          *s;
	td_request_t            treq;
	int                     pending;
	int                     error;
};

struct llecache {
	td_image_t             *shared;
	int                     mode;

	td_llecache_req_t       reqv[TD_LLECACHE_MAX_REQ];
	td_llecache_req_t      *free[TD_LLECACHE_MAX_REQ];
	int                     n_free;
};

static td_llecache_req_t *
llecache_alloc_request(td_llecache_t *s)
{
	td_llecache_req_t *req = NULL;

	if (likely(s->n_free))
		req = s->free[--s->n_free];

	return req;
}

static void
llecache_free_request(td_llecache_t *s, td_llecache_req_t *req)
{
	BUG_ON(s->n_free >= TD_LLECACHE_MAX_REQ);
	s->free[s->n_free++] = req;
}

static int
llecache_close(td_driver_t *driver)
{
	td_llecache_t *s = driver->data;

	if (s->shared) {
		tapdisk_image_close(s->shared);
		s->shared = NULL;
	}

	return 0;
}

static int
llecache_open(td_driver_t *driver, const char *name, td_flag_t flags)
{
	td_llecache_t *s = driver->data;
	int i, err;

	s->mode = LLE_LOCAL;

	for (i = 0; i < TD_LLECACHE_MAX_REQ; i++)
		llecache_free_request(s, &s->reqv[i]);

	err = tapdisk_image_open(DISK_TYPE_VHD, name, flags, &s->shared);
	if (err)
		goto fail;

	driver->info = s->shared->driver->info;

	return 0;

fail:
	llecache_close(driver);
	return err;
}

static void
__llecache_write_cb(td_request_t treq, int error)
{
	td_llecache_req_t *req = treq.cb_data;
	td_llecache_t *s = req->s;

	BUG_ON(req->pending < treq.secs);

	req->pending -= treq.secs;
	req->error    = ll_write_error(req->error, error);

	if (req->pending)
		return;

	if (req->error == -ENOSPC) {
		ll_log_switch(DISK_TYPE_LLECACHE, req->error,
			      treq.image, s->shared);

		s->mode = LLE_SHARED;
		td_queue_write(s->shared, req->treq);

	} else
		td_complete_request(req->treq, error);

	llecache_free_request(s, req);
}

static void
llecache_forward_write(td_llecache_t *s, td_request_t treq)
{
	td_llecache_req_t *req;
	td_request_t clone;

	req = llecache_alloc_request(s);
	if (!req) {
		td_complete_request(treq, -EBUSY);
		return;
	}

	memset(req, 0, sizeof(req));

	req->treq       = treq;
	req->pending    = treq.secs;
	req->s          = s;

	clone           = treq;
	clone.cb        = __llecache_write_cb;
	clone.cb_data   = req;

	td_forward_request(clone);
}

static void
llecache_queue_write(td_driver_t *driver, td_request_t treq)
{
	td_llecache_t *s = driver->data;

	switch (s->mode) {
	case LLE_LOCAL:
		llecache_forward_write(s, treq);
		break;
	case LLE_SHARED:
		td_queue_write(s->shared, treq);
		break;
	}
}

static void
llecache_queue_read(td_driver_t *driver, td_request_t treq)
{
	td_llecache_t *s = driver->data;

	switch (s->mode) {
	case LLE_LOCAL:
		td_forward_request(treq);
		break;
	case LLE_SHARED:
		td_queue_read(s->shared, treq);
		break;
	default:
		BUG();
	}
}

struct tap_disk tapdisk_llecache = {
	.disk_type                  = "tapdisk_llecache",
	.flags                      = 0,
	.private_data_size          = sizeof(td_llecache_t),
	.td_open                    = llecache_open,
	.td_close                   = llecache_close,
	.td_queue_read              = llecache_queue_read,
	.td_queue_write             = llecache_queue_write,
	.td_get_parent_id           = llcache_get_parent_id,
	.td_validate_parent         = llcache_validate_parent,
};
