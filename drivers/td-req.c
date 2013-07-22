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

#include "td-req.h"
#include "td-blkif.h"
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include "td-ctx.h"
#include <syslog.h>
#include <inttypes.h>
#include "tapdisk-vbd.h"

/*
 * TODO needed for PROT_READ/PROT_WRITE, probably some xen header supplies them
 * too
 */
#include <sys/mman.h>

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
    assert(blkif);
    assert(tapreq);
    assert(blkif->n_reqs_free <= blkif->ring_size);

    blkif->reqs_free[blkif->ring_size - (++blkif->n_reqs_free)] = &tapreq->msg;
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
    assert(blkif);

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
 * Unmaps a request's data.
 *
 * @param blkif the block interface the request belongs to
 * @param req the request to unmap
 */
static int
xenio_blkif_munmap_one(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const req)
{
    struct td_xenio_ctx *ctx;
    int err;

    assert(blkif);
    assert(req);

    ctx = blkif->ctx;
    assert(ctx);

    err = xc_gnttab_munmap(ctx->xcg_handle, req->vma, req->nr_segments);
    if (err) {
        err = errno;
        /* TODO don't use syslog for error on the data path */
        syslog(LOG_ERR, "failed to unmap pages: %s\n", strerror(err));
        return err;
    }

    req->vma = NULL;
    return 0;
}

/**
 * Get the response that corresponds to the specified ring index in a H/W
 * independent way.
 *
 * TODO use function pointers instead of switch
 * XXX only called by xenio_blkif_put_response
 */
static inline
blkif_response_t *xenio_blkif_get_response(struct td_xenblkif* const blkif,
        const RING_IDX rp)
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
            /* TODO gracefully fail? */
            abort();
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
        assert(msg);

        assert(status == BLKIF_RSP_EOPNOTSUPP || status == BLKIF_RSP_ERROR
                || status == BLKIF_RSP_OKAY);

        msg->id = req->id;

        /* TODO Why do we have to set this? */
        msg->operation = req->op;

        msg->status = status;

        ring->rsp_prod_pvt++;
    }

    if (final) {
        int notify;
        RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(ring, notify);
        if (notify) {
            int err = xc_evtchn_notify(blkif->ctx->xce_handle, blkif->port);
            if (err < 0) {
                /* TODO log error */
                return errno;
            }
        }
    }

    return 0;
}

/**
 * Completes a request.
 *
 * @blkif the VBD the request belongs belongs to
 * @tapreq the request to complete
 * @error completion status of the request
 * @final controls whether the other end should be notified
 */
static void
tapdisk_xenblkif_complete_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req* tapreq, const int error, const int final)
{
    assert(blkif);
    assert(tapreq);

    if (tapreq->vma) {
        xenio_blkif_munmap_one(blkif, tapreq);
        /* TODO How do we deal with errors here? */
    }

    xenio_blkif_put_response(blkif, tapreq,
            (error ? BLKIF_RSP_ERROR : BLKIF_RSP_OKAY), final);

    tapdisk_xenblkif_free_request(blkif, tapreq);

    blkif->stats.reqs.out++;
    if (final)
        blkif->stats.kicks.out++;
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

    assert(vreq);
    assert(blkif);

    tapreq = containerof(vreq, struct td_xenblkif_req, vreq);

    tapdisk_xenblkif_complete_request(blkif, tapreq, error, final);
    if (error)
        blkif->stats.errors.img++;
}

/**
 * Initialises the standard tapdisk request (td_vbd_request_t) from the
 * intermediate ring request (td_xenblkif_req) in order to prepare it
 * processing.
 *
 * @param blkif the block interface
 * @param tapreq the request to prepare
 * @returns 0 on success
 *
 * TODO only called by tapdisk_xenblkif_queue_request
 */
static inline int
tapdisk_xenblkif_make_vbd_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const tapreq)
{
    td_vbd_request_t *vreq;
    int i;
    struct td_iovec *iov;
    void *page, *next, *last;
    int prot;
    grant_ref_t gref[BLKIF_MAX_SEGMENTS_PER_REQUEST];

    assert(tapreq);

    vreq = &tapreq->vreq;
    assert(vreq);

    switch (tapreq->msg.operation) {
    case BLKIF_OP_READ:
        tapreq->op = BLKIF_OP_READ;
        vreq->op = TD_OP_READ;
        prot = PROT_WRITE;
        break;
    case BLKIF_OP_WRITE:
        tapreq->op = BLKIF_OP_WRITE;
        vreq->op = TD_OP_WRITE;
        prot = PROT_READ;
        break;
    default:
        /* TODO log error */
        return EOPNOTSUPP;
    }

    /* TODO there should be at least one segment, right? */
    if (tapreq->msg.nr_segments < 1
            || tapreq->msg.nr_segments > BLKIF_MAX_SEGMENTS_PER_REQUEST) {
        /* TODO log error */
        return EINVAL;
    }

    for (i = 0; i < tapreq->msg.nr_segments; i++) {
        struct blkif_request_segment *seg = &tapreq->msg.seg[i];
        gref[i] = seg->gref;

        /*
         * Note that first and last may be equal, which means only one sector
         * must be transferred.
         */
        if (seg->last_sect < seg->first_sect) {
            /* TODO log error */
            return EINVAL;
        }
    }

    tapreq->nr_segments = tapreq->msg.nr_segments;

    /*
     * Map the request's data.
     */
    tapreq->vma = xc_gnttab_map_domain_grant_refs(blkif->ctx->xcg_handle,
            tapreq->nr_segments, blkif->domid, gref, prot);
    if (!tapreq->vma) {
        /* TODO log error */
        return errno;
    }

    tapreq->id = tapreq->msg.id;

    /*
     * Vectorizes the request: creates the struct iovec (in tapreq->iov) that
     * describes each segment to be transferred. Also, merges consecutive
     * segments.
     * TODO The following piece of code would be much simpler if we didn't
     * merge segments, right?
     *
     * In each loop, iov points to the previous scatter/gather element in order
     * to reuse it if the current and previous segments are consecutive.
     */
    iov = tapreq->iov - 1;
    last = NULL;
    page = tapreq->vma;

    for (i = 0; i < tapreq->nr_segments; i++) { /* for each segment */
        struct blkif_request_segment *seg = &tapreq->msg.seg[i];
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
    }

    vreq->iov = tapreq->iov;
    vreq->iovcnt = iov - tapreq->iov + 1;
    vreq->sec = tapreq->msg.sector_number;

    /*
     * TODO Isn't this kind of expensive to do for each requests? Why does the
     * tapdisk need this in the first place?
     */
    snprintf(tapreq->name, sizeof(tapreq->name), "xenvbd-%d-%d.%"SCNx64"",
             blkif->domid, blkif->devid, tapreq->msg.id);

    vreq->name = tapreq->name;
    vreq->token = blkif;
    vreq->cb = __tapdisk_xenblkif_request_cb;

    return 0;
}

#define msg_to_tapreq(_req) \
	containerof(_req, struct td_xenblkif_req, msg)

/**
 * Queues a ring request, after it prepares it, to the standard taodisk queue
 * for processing.
 *
 * @param blkif the block interface
 * @param msg the ring request
 * @param tapreq the intermediate request
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

    assert(blkif);
    assert(msg);
    assert(tapreq);

    err = tapdisk_xenblkif_make_vbd_request(blkif, tapreq);
    if (err) {
        /* TODO log error */
        blkif->stats.errors.map++;
        return err;
    }

    err = tapdisk_vbd_queue_request(blkif->vbd, &tapreq->vreq);
    if (err) {
        /* TODO log error */
        blkif->stats.errors.vbd++;
        return err;
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

    assert(blkif);
    assert(reqs);
    assert(nr_reqs >= 0);

    for (i = 0; i < nr_reqs; i++) { /* for each request from the ring... */
        blkif_request_t *msg = reqs[i];
        struct td_xenblkif_req *tapreq;

        assert(msg);

        tapreq = msg_to_tapreq(msg);

        assert(tapreq);

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
    assert(blkif);

    free(blkif->reqs);
    blkif->reqs = NULL;

    free(blkif->reqs_free);
    blkif->reqs_free = NULL;
}

int
tapdisk_xenblkif_reqs_init(struct td_xenblkif *td_blkif)
{
    int i = 0;
    int err = 0;

    assert(td_blkif);

    td_blkif->ring_size = td_blkif_ring_size(td_blkif);
    assert(td_blkif->ring_size > 0);

    td_blkif->reqs =
        malloc(td_blkif->ring_size * sizeof(struct td_xenblkif_req));
    if (!td_blkif->reqs) {
        /* TODO log error */
        err = -errno;
        goto fail;
    }

    td_blkif->reqs_free =
        malloc(td_blkif->ring_size * sizeof(struct xenio_blkif_req *));
    if (!td_blkif->reqs_free) {
        /* TODO log error */
        err = -errno;
        goto fail;
    }

    td_blkif->n_reqs_free = 0;
    for (i = 0; i < td_blkif->ring_size; i++)
        tapdisk_xenblkif_free_request(td_blkif, &td_blkif->reqs[i]);

    return 0;

fail:
    tapdisk_xenblkif_reqs_free(td_blkif);
    return err;
}
