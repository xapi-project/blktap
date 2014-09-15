/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <xenctrl.h>

#include <stdlib.h>
#include <errno.h>
#include <syslog.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "debug.h"
#include "td-req.h"
#include "td-blkif.h"
#include "td-ctx.h"
#include "tapdisk-server.h"
#include "tapdisk-vbd.h"
#include "tapdisk-log.h"
#include "tapdisk.h"
#include "util.h"

#ifdef DEBUG
#define BLKIF_MSG_POISON 0xdeadbeef
#endif

#define ERR(blkif, fmt, args...) \
    EPRINTF("%d/%d: "fmt, (blkif)->domid, (blkif)->devid, ##args);

#define TD_REQS_BUFCACHE_EXPIRE 3 // time in seconds
#define TD_REQS_BUFCACHE_MIN    1 // buffers to always keep in the cache

static void
td_xenblkif_bufcache_free(struct td_xenblkif * const blkif);
static inline void
td_xenblkif_bufcache_evt_unreg(struct td_xenblkif * const blkif);

static void
td_xenblkif_bufcache_event(event_id_t id, char mode, void *private)
{
    struct td_xenblkif *blkif = private;

    td_xenblkif_bufcache_free(blkif);

    td_xenblkif_bufcache_evt_unreg(blkif);
}

/**
 * Unregister the event to expire the request buffer cache.
 *
 * @param blkif the block interface
 */
static inline void
td_xenblkif_bufcache_evt_unreg(struct td_xenblkif * const blkif)
{
    if (blkif->reqs_bufcache_evtid > 0){
        tapdisk_server_unregister_event(blkif->reqs_bufcache_evtid);
    }
    blkif->reqs_bufcache_evtid = 0;
}

/**
 * Register the event to expire the request buffer cache.
 *
 * @param blkif the block interface
 */
static inline void
td_xenblkif_bufcache_evt_reg(struct td_xenblkif * const blkif)
{
    blkif->reqs_bufcache_evtid =
        tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT,
                                      -1, /* dummy fd */
                                      TD_REQS_BUFCACHE_EXPIRE,
                                      td_xenblkif_bufcache_event,
                                      blkif);
}

/**
 * Free request buffer cache.
 *
 * @param blkif the block interface
 */
static void
td_xenblkif_bufcache_free(struct td_xenblkif * const blkif)
{
    ASSERT(blkif);

    while (blkif->n_reqs_bufcache_free > TD_REQS_BUFCACHE_MIN){
        munmap(blkif->reqs_bufcache[--blkif->n_reqs_bufcache_free],
               BLKIF_MAX_SEGMENTS_PER_REQUEST << XC_PAGE_SHIFT);
    }
}

/**
 * Get buffer for a request. From cache if available or newly allocated.
 *
 * @param blkif the block interface
 */
static void *
td_xenblkif_bufcache_get(struct td_xenblkif * const blkif)
{
    void *buf;

    ASSERT(blkif);

    if (!blkif->n_reqs_bufcache_free) {
        buf = mmap(NULL, BLKIF_MAX_SEGMENTS_PER_REQUEST << XC_PAGE_SHIFT,
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (unlikely(buf == MAP_FAILED))
            buf = NULL;
    } else
        buf = blkif->reqs_bufcache[--blkif->n_reqs_bufcache_free];

    // If we just got a request, we cancel the cache expire timer
    td_xenblkif_bufcache_evt_unreg(blkif);

    return buf;
}

static void
td_xenblkif_bufcache_put(struct td_xenblkif * const blkif, void *buf)
{
    ASSERT(blkif);

    if (unlikely(!buf))
        return;

#ifdef DEBUG
	{
		int i;

		for (i = 0; i < blkif->n_reqs_bufcache_free; i++)
			ASSERT(blkif->reqs_bufcache[i] != buf);
	}
#endif

    blkif->reqs_bufcache[blkif->n_reqs_bufcache_free++] = buf;

    /* If we're in low memory mode, prune the bufcache immediately. */
    if (tapdisk_server_mem_mode() == LOW_MEMORY_MODE) {
        td_xenblkif_bufcache_free(blkif);
    } else {
        // We only set the expire event when no requests are inflight
        if (blkif->n_reqs_free == blkif->ring_size)
            td_xenblkif_bufcache_evt_reg(blkif);
    }
}

/**
 * Puts the request back to the free list of this block interface.
 *
 * @param blkif the block interface
 * @param tapreq the request to give back
 */
static void
tapdisk_xenblkif_free_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const tapreq)
{
    ASSERT(blkif);
    ASSERT(tapreq);
    ASSERT(blkif->n_reqs_free < blkif->ring_size);

#ifdef DEBUG
	memset(&tapreq->msg, BLKIF_MSG_POISON, sizeof(tapreq->msg));
#endif

    blkif->reqs_free[blkif->ring_size - (++blkif->n_reqs_free)] = &tapreq->msg;

	if (likely(tapreq->msg.nr_segments))
	    td_xenblkif_bufcache_put(blkif, tapreq->vma);
}

/**
 * Returns the size, in request descriptors, of the shared ring
 *
 * @param blkif the block interface
 * @returns the size, in request descriptors, of the shared ring
 */
static int
td_blkif_ring_size(const struct td_xenblkif * const blkif)
{
    ASSERT(blkif);

    switch (blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            return RING_SIZE(&blkif->rings.native);

        case BLKIF_PROTOCOL_X86_32:
            return RING_SIZE(&blkif->rings.x86_32);

        case BLKIF_PROTOCOL_X86_64:
            return RING_SIZE(&blkif->rings.x86_64);

        default:
            return -EPROTONOSUPPORT;
    }
}

/**
 * Get the response that corresponds to the specified ring index in a H/W
 * independent way.
 *
 * @returns a pointer to the response, NULL on error, sets errno
 *
 * TODO use function pointers instead of switch
 * XXX only called by xenio_blkif_put_response
 */
static inline blkif_response_t *
xenio_blkif_get_response(struct td_xenblkif* const blkif, const RING_IDX rp)
{
    blkif_back_rings_t * const rings = &blkif->rings;
    blkif_response_t * p = NULL;

    switch (blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->native, rp);
            break;
        case BLKIF_PROTOCOL_X86_32:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->x86_32, rp);
            break;
        case BLKIF_PROTOCOL_X86_64:
            p = (blkif_response_t *) RING_GET_RESPONSE(&rings->x86_64, rp);
            break;
        default:
            errno = EPROTONOSUPPORT;
			return NULL;
    }

    return p;
}

/**
 * Puts a response in the ring.
 *
 * @param blkif the VBD
 * @param req the request for which the response should be put
 * @param status the status of the response (success or an error code)
 * @param final controls whether the front-end will be notified, if necessary
 *
 * TODO @req can be NULL so the function will only notify the other end. This
 * is used in the error path of tapdisk_xenblkif_queue_requests. The point is
 * that the other will just be notified, does this make sense?
 */
static int
xenio_blkif_put_response(struct td_xenblkif * const blkif,
        struct td_xenblkif_req *req, int const status, int const final)
{
    blkif_common_back_ring_t * const ring = &blkif->rings.common;

    if (req) {
        blkif_response_t * msg = xenio_blkif_get_response(blkif,
                ring->rsp_prod_pvt);
		if (!msg)
			return -errno;

        ASSERT(status == BLKIF_RSP_EOPNOTSUPP || status == BLKIF_RSP_ERROR
                || status == BLKIF_RSP_OKAY);

        msg->id = req->msg.id;

        msg->operation = req->msg.operation;

        msg->status = status;

        ring->rsp_prod_pvt++;
    }

    if (final) {
        int notify;
        RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(ring, notify);
        if (notify) {
            int err = xc_evtchn_notify(blkif->ctx->xce_handle, blkif->port);
            if (err < 0) {
                err = -errno;
                if (req) {
                    RING_ERR(blkif, "req %lu: failed to notify event channel: "
                            "%s\n", req->msg.id, strerror(-err));
                } else {
                    RING_ERR(blkif, "failed to notify event channel: %s\n",
                            strerror(-err));
                }
                return err;
            }
        }
    }

    return 0;
}


/**
 * Tells whether the request requires data to be read.
 */
static inline bool
blkif_rq_rd(blkif_request_t const * const msg)
{
	return BLKIF_OP_READ == msg->operation;
}


/**
 * Tells whether the request requires data to be written.
 */
static inline bool
blkif_rq_wr(blkif_request_t const * const msg)
{
	return BLKIF_OP_WRITE == msg->operation ||
		(BLKIF_OP_WRITE_BARRIER == msg->operation && msg->nr_segments);
}


/**
 * Tells whether the request requires data to transferred.
 */
static inline bool
blkif_rq_data(blkif_request_t const * const msg)
{
	return blkif_rq_rd(msg) || blkif_rq_wr(msg);
}


static int
guest_copy2(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const tapreq /* TODO rename to req */) {

    int i = 0;
    long err = 0;
    struct ioctl_gntdev_grant_copy gcopy;

    ASSERT(blkif);
    ASSERT(blkif->ctx);
    ASSERT(tapreq);
    ASSERT(blkif_rq_data(&tapreq->msg));
	ASSERT(tapreq->msg.nr_segments > 0);
	ASSERT(tapreq->msg.nr_segments <= ARRAY_SIZE(tapreq->gcopy_segs));

    for (i = 0; i < tapreq->msg.nr_segments; i++) {
        struct blkif_request_segment *blkif_seg = &tapreq->msg.seg[i];
        struct gntdev_grant_copy_segment *gcopy_seg = &tapreq->gcopy_segs[i];
        gcopy_seg->iov.iov_base = tapreq->vma + (i << PAGE_SHIFT)
            + (blkif_seg->first_sect << SECTOR_SHIFT);
        gcopy_seg->iov.iov_len = (blkif_seg->last_sect
                - blkif_seg->first_sect
                + 1)
            << SECTOR_SHIFT;
        gcopy_seg->ref = blkif_seg->gref;
        gcopy_seg->offset = blkif_seg->first_sect << SECTOR_SHIFT;
    }

    gcopy.dir = blkif_rq_wr(&tapreq->msg);
    gcopy.domid = blkif->domid;
    gcopy.count = tapreq->msg.nr_segments;
	gcopy.segments = tapreq->gcopy_segs;

    err = -ioctl(blkif->ctx->gntdev_fd, IOCTL_GNTDEV_GRANT_COPY, &gcopy);
    if (err) {
        err = -errno;
        RING_ERR(blkif, "failed to grant-copy request %"PRIu64" "
                "(%d segments): %s\n", tapreq->msg.id,
                tapreq->msg.nr_segments, strerror(-err));
        goto out;
    }

	for (i = 0; i < tapreq->msg.nr_segments; i++) {
		struct gntdev_grant_copy_segment *gcopy_seg = &tapreq->gcopy_segs[i];
		if (gcopy_seg->status != GNTST_okay) {
			/*
			 * TODO use gnttabop_error for reporting errors, defined in
			 * xen/extras/mini-os/include/gnttab.h (header not available to
			 * user space)
			 */
			RING_ERR(blkif, "req %lu: failed to grant-copy segment %d: %d\n",
                    tapreq->msg.id, i, gcopy_seg->status);
			err = -EIO;
			goto out;
		}
	}

out:
    return err;
}


/**
 * Completes a request. If this is the last pending request of a dead block
 * interface, the block interface is destroyed, the caller must not access it
 * any more.
 *
 * @blkif the VBD the request belongs belongs to
 * @tapreq the request to complete TODO rename to req
 * @error completion status of the request
 * @final controls whether the other end should be notified
 */
void
tapdisk_xenblkif_complete_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req* tapreq, int err, const int final)
{
	int _err;
    long long *max = NULL, *sum = NULL, *cnt = NULL;
	static int depth = 0;
	bool processing_barrier_message;

    ASSERT(blkif);
    ASSERT(tapreq);
	ASSERT(depth >= 0);

	depth++;

	processing_barrier_message =
		tapreq->msg.operation == BLKIF_OP_WRITE_BARRIER;

	/*
	 * If a barrier request completes, check whether it's an I/O completion
	 * (the barrier carries write I/O data), or a completion because the last
	 * pending non-barrier request completed. If the former case is true, we
	 * need to check again whether the latter is true and proceed with the
	 * completion, otherwise simply note the fact that I/O is done and when
	 * the last pending non-barrier requests completes, this function will be
	 * called again passing the barrier request.
	 */
	if (unlikely(processing_barrier_message)) {
		ASSERT(blkif->barrier.msg == &tapreq->msg);
		if (tapreq->msg.nr_segments && !blkif->barrier.io_done) {
			blkif->barrier.io_err = err;
			blkif->barrier.io_done = true;
		}
		if (!tapdisk_xenblkif_barrier_should_complete(blkif))
			goto out;
	}

	if (likely(!blkif->dead)) {
		if (blkif_rq_rd(&tapreq->msg)) {
			/*
			 * TODO stats should be collected after grant-copy for better
			 * accuracy
			 */
			cnt = &blkif->stats.xenvbd->st_rd_cnt;
			sum = &blkif->stats.xenvbd->st_rd_sum_usecs;
			max = &blkif->stats.xenvbd->st_rd_max_usecs;
			if (likely(!err)) {
				_err = guest_copy2(blkif, tapreq);
				if (unlikely(_err)) {
					err = _err;
					RING_ERR(blkif, "req %lu: failed to copy from/to guest: "
							"%s\n", tapreq->msg.id, strerror(-err));
				}
			}
		} else if (blkif_rq_wr(&tapreq->msg)) {
			cnt = &blkif->stats.xenvbd->st_wr_cnt;
			sum = &blkif->stats.xenvbd->st_wr_sum_usecs;
			max = &blkif->stats.xenvbd->st_wr_max_usecs;
		}

		if (likely(cnt)) {
			struct timeval now;
			long long interval;
			gettimeofday(&now, NULL);
			interval = timeval_to_us(&now) - timeval_to_us(&tapreq->vreq.ts);

			if (interval > *max)
				*max = interval;

			*sum += interval;
			*cnt += 1;
		}

		if (likely(err == 0))
            _err = BLKIF_RSP_OKAY;
		else
            _err = BLKIF_RSP_ERROR;

		xenio_blkif_put_response(blkif, tapreq, _err, final);
	}

    tapdisk_xenblkif_free_request(blkif, tapreq);

    blkif->stats.reqs.out++;
    if (final)
        blkif->stats.kicks.out++;

	if (unlikely(processing_barrier_message))
		blkif->barrier.msg = NULL;

    /*
	 * Schedule a ring check in case we left requests in it due to lack of
	 * memory or in case we stopped processing it because of a barrier.
	 *
	 * FIXME we should decide whether a ring check is necessary more
	 * intelligently.
	*/
	if (!blkif->barrier.msg) {
		if (likely(!blkif->dead))
			tapdisk_xenblkif_sched_chkrng(blkif);
	} else {
		/*
		 * If this is the last request, complete the barrier request.
		 */
		if (tapdisk_xenblkif_barrier_should_complete(blkif))
			tapdisk_xenblkif_complete_request(blkif,
					msg_to_tapreq(blkif->barrier.msg), 0, 1);
	}

	/*
	 * Last request of a dead ring completes, destroy the ring.
	 */
	if (unlikely(1 == depth
				&& blkif->dead
				&& !tapdisk_xenblkif_reqs_pending(blkif))) {

		RING_DEBUG(blkif, "destroying dead ring\n");
		tapdisk_xenblkif_destroy(blkif);
	}

out:
	depth--;
}

/**
 * Request completion callback, executed when the tapdisk has finished
 * processing the request.
 *
 * @param vreq the completed request
 * @param error status of the request
 * @param token token previously associated with this request
 * @param final TODO ?
 */
static inline void
__tapdisk_xenblkif_request_cb(struct td_vbd_request * const vreq,
        const int error, void * const token, const int final)
{
    struct td_xenblkif_req *tapreq;
    struct td_xenblkif * const blkif = token;

    ASSERT(vreq);
    ASSERT(blkif);

    tapreq = containerof(vreq, struct td_xenblkif_req, vreq);

    if (error)
        blkif->stats.errors.img++;
    tapdisk_xenblkif_complete_request(blkif, tapreq, error, final);
}


static inline int
tapdisk_xenblkif_parse_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const req)
{
    td_vbd_request_t *vreq;
    int i;
    struct td_iovec *iov;
    void *page, *next, *last;
    int err = 0;
    unsigned nr_sect = 0;

    ASSERT(blkif);
    ASSERT(req);

    vreq = &req->vreq;
    ASSERT(vreq);

    req->vma = td_xenblkif_bufcache_get(blkif);
    if (unlikely(!req->vma)) {
        err = errno;
        goto out;
    }

    for (i = 0; i < req->msg.nr_segments; i++) {
        struct blkif_request_segment *seg = &req->msg.seg[i];
        req->gref[i] = seg->gref;

        /*
         * Note that first and last may be equal, which means only one sector
         * must be transferred.
         */
        if (seg->last_sect < seg->first_sect) {
            RING_ERR(blkif, "req %lu: invalid sectors %d-%d\n",
                    req->msg.id, seg->first_sect, seg->last_sect);
            err = EINVAL;
            goto out;
        }
    }

    /*
     * Vectorises the request: creates the struct iovec (in tapreq->iov) that
     * describes each segment to be transferred. Also, merges consecutive
     * segments.
     *
     * In each loop, iov points to the previous scatter/gather element in
     * order to reuse it if the current and previous segments are
     * consecutive.
     */
    iov = req->iov - 1;
    last = NULL;
    page = req->vma;

    for (i = 0; i < req->msg.nr_segments; i++) { /* for each segment */
        struct blkif_request_segment *seg = &req->msg.seg[i];
        size_t size;

        /* TODO check that first_sect/last_sect are within page */

        next = page + (seg->first_sect << SECTOR_SHIFT);
        size = seg->last_sect - seg->first_sect + 1;

        if (next != last) {
            iov++;
            iov->base = next;
            iov->secs = size;
        } else /* The "else" is true if fist_sect is 0. */
            iov->secs += size;

        last = iov->base + (iov->secs << SECTOR_SHIFT);
        page += XC_PAGE_SIZE;
        nr_sect += size;
    }

    vreq->iov = req->iov;
    vreq->iovcnt = iov - req->iov + 1;
    vreq->sec = req->msg.sector_number;

    if (blkif_rq_wr(&req->msg)) {
        err = guest_copy2(blkif, req);
        if (err) {
            RING_ERR(blkif, "req %lu: failed to copy from guest: %s\n",
                    req->msg.id, strerror(-err));
            goto out;
        }
        blkif->stats.xenvbd->st_wr_sect += nr_sect;
    } else
        blkif->stats.xenvbd->st_rd_sect += nr_sect;

    /*
     * TODO Isn't this kind of expensive to do for each requests? Why does
     * the tapdisk need this in the first place?
     */
    snprintf(req->name, sizeof(req->name), "xenvbd-%d-%d.%"SCNx64"",
             blkif->domid, blkif->devid, req->msg.id);

    vreq->name = req->name;
    vreq->token = blkif;
    vreq->cb = __tapdisk_xenblkif_request_cb;

out:
    return err;
}


/**
 * Initialises the standard tapdisk request (td_vbd_request_t) from the
 * intermediate ring request (td_xenblkif_req) in order to prepare it
 * processing.
 *
 * @param blkif the block interface
 * @param tapreq the request to prepare TODO rename to req
 * @returns 0 on success
 *
 * XXX only called by tapdisk_xenblkif_queue_request
 */
static inline int
tapdisk_xenblkif_make_vbd_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const tapreq)
{
    int err = 0;
    td_vbd_request_t *vreq;

    ASSERT(tapreq);

    vreq = &tapreq->vreq;
    ASSERT(vreq);
    memset(vreq, 0, sizeof(*vreq));

	tapreq->vma = NULL;

    switch (tapreq->msg.operation) {
    case BLKIF_OP_READ:
        blkif->stats.xenvbd->st_rd_req++;
        tapreq->prot = PROT_WRITE;
        vreq->op = TD_OP_READ;
        break;
    case BLKIF_OP_WRITE:
    case BLKIF_OP_WRITE_BARRIER:
        blkif->stats.xenvbd->st_wr_req++;
        tapreq->prot = PROT_READ;
        vreq->op = TD_OP_WRITE;
        break;
    default:
        RING_ERR(blkif, "req %lu: invalid request type %d\n",
                tapreq->msg.id, tapreq->msg.operation);
        err = EOPNOTSUPP;
        goto out;
    }

    /*
     * Check that the number of segments is sane.
     */
    if (unlikely((tapreq->msg.nr_segments == 0 &&
                tapreq->msg.operation != BLKIF_OP_WRITE_BARRIER) ||
            tapreq->msg.nr_segments > BLKIF_MAX_SEGMENTS_PER_REQUEST)) {
        RING_ERR(blkif, "req %lu: bad number of segments in request (%d)\n",
                tapreq->msg.id, tapreq->msg.nr_segments);
        err = EINVAL;
        goto out;
    }

    if (likely(tapreq->msg.nr_segments))
        err = tapdisk_xenblkif_parse_request(blkif, tapreq);
    /*
     * If we only got one request from the ring and that was a barrier one,
     * check whether the barrier requests completion conditions are satisfied
	 * and if they are, complete the barrier request.
     *
     * It could be that there are more requests in the ring after the barrier
     * request, tapdisk_xenblkif_complete_request() will schedule a ring check.
     */
    else if (tapdisk_xenblkif_barrier_should_complete(blkif)) {
        tapdisk_xenblkif_complete_request(blkif,
                msg_to_tapreq(blkif->barrier.msg), 0, 1);
        err = 0;
    }
out:
    return err;
}


/**
 * Queues a ring request, after it prepares it, to the standard taodisk queue
 * for processing.
 *
 * @param blkif the block interface
 * @param msg the ring request
 * @param tapreq the intermediate request TODO rename to req
 *
 * TODO don't really need to supply the ring request since it's either way
 * contained in the tapreq
 *
 * XXX only called by tapdisk_xenblkif_queue_requests
 */
static inline int
tapdisk_xenblkif_queue_request(struct td_xenblkif * const blkif,
        blkif_request_t *msg, struct td_xenblkif_req *tapreq)
{
    int err;

    ASSERT(blkif);
    ASSERT(msg);
    ASSERT(tapreq);

    err = tapdisk_xenblkif_make_vbd_request(blkif, tapreq);
    if (unlikely(err)) {
        /* TODO log error */
        blkif->stats.errors.map++;
        return err;
    }

	if (likely(tapreq->msg.nr_segments)) {
		err = tapdisk_vbd_queue_request(blkif->vbd, &tapreq->vreq);
		if (unlikely(err)) {
			/* TODO log error */
			blkif->stats.errors.vbd++;
			return err;
		}
	}

    return 0;
}


void
tapdisk_xenblkif_queue_requests(struct td_xenblkif * const blkif,
        blkif_request_t *reqs[], const int nr_reqs)
{
    int i;
    int err;
    int nr_errors = 0;

    ASSERT(blkif);
    ASSERT(reqs);
    ASSERT(nr_reqs >= 0);

    for (i = 0; i < nr_reqs; i++) { /* for each request in the ring... */
        blkif_request_t *msg = reqs[i];
        struct td_xenblkif_req *tapreq;

        ASSERT(msg);

        tapreq = msg_to_tapreq(msg);

        ASSERT(tapreq);

        err = tapdisk_xenblkif_queue_request(blkif, msg, tapreq);
        if (err) {
            /* TODO log error */
            nr_errors++;
            tapdisk_xenblkif_complete_request(blkif, tapreq, err, 1);
        }
    }

    if (nr_errors)
        xenio_blkif_put_response(blkif, NULL, 0, 1);
}

void
tapdisk_xenblkif_reqs_free(struct td_xenblkif * const blkif)
{
    ASSERT(blkif);

    td_xenblkif_bufcache_free(blkif);
    td_xenblkif_bufcache_evt_unreg(blkif);

    free(blkif->reqs);
    blkif->reqs = NULL;

    free(blkif->reqs_free);
    blkif->reqs_free = NULL;

}

int
tapdisk_xenblkif_reqs_init(struct td_xenblkif *td_blkif)
{
    void *buf;
    int i = 0;
    int err = 0;

    ASSERT(td_blkif);

    td_blkif->ring_size = td_blkif_ring_size(td_blkif);
    ASSERT(td_blkif->ring_size > 0);

    td_blkif->reqs =
        calloc(td_blkif->ring_size, sizeof(struct td_xenblkif_req));
    if (!td_blkif->reqs) {
        err = -errno;
        goto fail;
    }

    td_blkif->reqs_free =
        malloc(td_blkif->ring_size * sizeof(struct xenio_blkif_req *));
    if (!td_blkif->reqs_free) {
        err = -errno;
        goto fail;
    }

    td_blkif->n_reqs_free = 0;
    for (i = 0; i < td_blkif->ring_size; i++)
        tapdisk_xenblkif_free_request(td_blkif, &td_blkif->reqs[i]);

    // Allocate the buffer cache
    td_blkif->reqs_bufcache = malloc(sizeof(void*) * td_blkif->ring_size);
    if (!td_blkif->reqs_bufcache) {
        err = -errno;
        goto fail;
    }
    td_blkif->n_reqs_bufcache_free = 0;
    td_blkif->reqs_bufcache_evtid = 0;

    // Populate cache with one buffer
    buf = td_xenblkif_bufcache_get(td_blkif);
    td_xenblkif_bufcache_put(td_blkif, buf);
    td_xenblkif_bufcache_evt_unreg(td_blkif);

    return 0;

fail:
    tapdisk_xenblkif_reqs_free(td_blkif);
    return err;
}
