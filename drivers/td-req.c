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
#include "tapdisk-vbd.h"
#include "tapdisk-log.h"
#include "tapdisk.h"
#include "util.h"

#define ERR(blkif, fmt, args...) \
    EPRINTF("%d/%d: "fmt, (blkif)->domid, (blkif)->devid, ##args);

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
    ASSERT(blkif->n_reqs_free <= blkif->ring_size);

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

        /* TODO Why do we have to set this? */
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
                ERR(blkif, "failed to notify event channel: %s\n",
                        strerror(-err));
                return err;
            }
        }
    }

    return 0;
}

static int
guest_copy2(struct td_xenblkif * const blkif,
        struct td_xenblkif_req * const tapreq) {

    int i = 0;
    td_vbd_request_t *vreq = NULL;
    long err = 0;
    struct ioctl_gntdev_grant_copy gcopy;

    ASSERT(blkif);
    ASSERT(blkif->ctx);
    ASSERT(tapreq);
    ASSERT(BLKIF_OP_READ == tapreq->msg.operation
			|| BLKIF_OP_WRITE == tapreq->msg.operation);
	ASSERT(tapreq->msg.nr_segments > 0);
	ASSERT(tapreq->msg.nr_segments <= ARRAY_SIZE(tapreq->gcopy_segs));

    vreq = &tapreq->vreq;

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

    gcopy.dir = BLKIF_OP_WRITE == tapreq->msg.operation;
    gcopy.domid = blkif->domid;
    gcopy.count = tapreq->msg.nr_segments;
	gcopy.segments = tapreq->gcopy_segs;

    err = -ioctl(blkif->ctx->gntdev_fd, IOCTL_GNTDEV_GRANT_COPY, &gcopy);
    if (err) {
        ERR(blkif, "failed to grant-copy: %s\n", strerror(err));
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
			ERR(blkif, "failed to grant-copy segment %d: %d\n", i,
					gcopy_seg->status);
			err = -EIO;
			goto out;
		}
	}

out:
    return err;
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
        struct td_xenblkif_req* tapreq, int err, const int final)
{
	int _err;

    ASSERT(blkif);
    ASSERT(tapreq);

	if (BLKIF_OP_READ == tapreq->msg.operation && !err) {
		_err = guest_copy2(blkif, tapreq);
		if (_err) {
			err = _err;
			ERR(blkif, "failed to copy from/to guest: %s\n",
					strerror(err));
		}
	}

    xenio_blkif_put_response(blkif, tapreq,
            (err ? BLKIF_RSP_ERROR : BLKIF_RSP_OKAY), final);

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

    ASSERT(vreq);
    ASSERT(blkif);

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
    int err = 0;

    ASSERT(tapreq);

    vreq = &tapreq->vreq;
    ASSERT(vreq);
    memset(vreq, 0, sizeof(*vreq));

    switch (tapreq->msg.operation) {
    case BLKIF_OP_READ:
        tapreq->prot = PROT_WRITE;
        vreq->op = TD_OP_READ;
        break;
    case BLKIF_OP_WRITE:
        tapreq->prot = PROT_READ;
        vreq->op = TD_OP_WRITE;
        break;
    default:
        ERR(blkif, "invalid request type %d\n", tapreq->msg.operation);
        err = EOPNOTSUPP;
        goto out;
    }

    /* TODO there should be at least one segment, right? */
    if (tapreq->msg.nr_segments < 1
            || tapreq->msg.nr_segments > BLKIF_MAX_SEGMENTS_PER_REQUEST) {
        ERR(blkif, "invalid segment count %d\n", tapreq->msg.nr_segments);
        err = EINVAL;
        goto out;
    }

    for (i = 0; i < tapreq->msg.nr_segments; i++) {
        struct blkif_request_segment *seg = &tapreq->msg.seg[i];
        tapreq->gref[i] = seg->gref;

        /*
         * Note that first and last may be equal, which means only one sector
         * must be transferred.
         */
        if (seg->last_sect < seg->first_sect) {
            ERR(blkif, "invalid sectors %d-%d\n", seg->first_sect,
                    seg->last_sect);
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
    iov = tapreq->iov - 1;
    last = NULL;
    page = tapreq->vma;

    for (i = 0; i < tapreq->msg.nr_segments; i++) { /* for each segment */
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

    if (tapreq->msg.operation == BLKIF_OP_WRITE) {
        err = guest_copy2(blkif, tapreq);
        if (err) {
            ERR(blkif, "failed to copy from guest: %s\n", strerror(err));
            goto out;
        }
    }

    /*
     * TODO Isn't this kind of expensive to do for each requests? Why does
     * the tapdisk need this in the first place?
     */
    snprintf(tapreq->name, sizeof(tapreq->name), "xenvbd-%d-%d.%"SCNx64"",
             blkif->domid, blkif->devid, tapreq->msg.id);

    vreq->name = tapreq->name;
    vreq->token = blkif;
    vreq->cb = __tapdisk_xenblkif_request_cb;

out:
    return err;
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

    ASSERT(blkif);
    ASSERT(msg);
    ASSERT(tapreq);

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

    ASSERT(blkif);
    ASSERT(reqs);
    ASSERT(nr_reqs >= 0);

    for (i = 0; i < nr_reqs; i++) { /* for each request from the ring... */
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
	int i;

    ASSERT(blkif);

	for (i = 0; i < blkif->ring_size; i++)
		free(blkif->reqs[i].vma);

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
    for (i = 0; i < td_blkif->ring_size; i++) {
		err = posix_memalign(&td_blkif->reqs[i].vma, XC_PAGE_SIZE,
				BLKIF_MAX_SEGMENTS_PER_REQUEST << XC_PAGE_SHIFT);
		if (err) {
			err = -errno;
			goto fail;
		}
        tapdisk_xenblkif_free_request(td_blkif, &td_blkif->reqs[i]);
	}

    return 0;

fail:
    tapdisk_xenblkif_reqs_free(td_blkif);
    return err;
}
