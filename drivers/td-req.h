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

#ifndef __TD_REQ_H__
#define __TD_REQ_H__

#include "tapdisk.h"
#include <sys/types.h>
#include <xen/io/blkif.h>
#include <xen/gntdev.h>
#include "td-blkif.h"

/**
 * Representation of the intermediate request used to retrieve a request from
 * the shared ring and handle it over to the main tapdisk request processing
 * routine.  We could merge it into td_vbd_request_t or define it inside
 * td_vbd_request_t, but keeping it separate simplifies keeping Xen stuff
 * outside tapdisk.
 *
 * TODO rename to something better, e.g. ring_req?
 */
struct td_xenblkif_req {
    /**
     * A request descriptor in the ring. We need to copy the descriptors
     * because the guest may modify it while we're using it. Note that we
     * only copy the descriptor and not the actual data, the guest is free
     * to modify the data and corrupt itself if it wants to.
     */
    blkif_request_t msg;

    /**
     * tapdisk's representation of the request.
     */
    td_vbd_request_t vreq;

    /**
     * Pointer to memory-mapped grant refs. We keep this around because we need
     * to pass it to xc_gnttab_munmap when the requests is completed.
     */
    void *vma;

    /*
     * TODO Why 16+1? This member is copied to the corresponding one in
     * td_vbd_request_t, so check the limit of that, if there is one.
     */
    char name[16 + 1];

    struct timeval ts;

    /**
     * The scatter/gather list td_vbd_request_t.iov points to.
     */
    struct td_iovec iov[BLKIF_MAX_SEGMENTS_PER_REQUEST];

    grant_ref_t gref[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    int prot;

	struct gntdev_grant_copy_segment
		gcopy_segs[BLKIF_MAX_SEGMENTS_PER_REQUEST];
};

struct td_xenblkif;

/**
 * Queues the requests to the standard tapdisk queue.
 *
 * @param td_blkif the block interface corresponding to the VBD
 * @param reqs array holding the request rescriptors
 * @param nr_reqs number of requests in the array
 */
void
tapdisk_xenblkif_queue_requests(struct td_xenblkif * const blkif,
        blkif_request_t *reqs[], const int nr_reqs);

/**
 * Initilises the intermediate requests of this block interface.
 *
 * @params td_blkif the block interface whose requests must be initialised
 * @returns 0 on success
 */
int
tapdisk_xenblkif_reqs_init(struct td_xenblkif *td_blkif);

/**
 * Releases all the requests of the block interface.
 *
 * @param blkif the block interface whose requests should be freed
 */
void
tapdisk_xenblkif_reqs_free(struct td_xenblkif * const blkif);

/**
 * Completes a request. If this is the last pending request of a dead block
 * interface, the block interface is destroyed, the caller must not access it
 * any more.
 *
 * @blkif the VBD the request belongs belongs to
 * @tapreq the request to complete
 * @error completion status of the request
 * @final controls whether the other end should be notified
 */

void
tapdisk_xenblkif_complete_request(struct td_xenblkif * const blkif,
        struct td_xenblkif_req* tapreq, int err, const int final);

#define msg_to_tapreq(_req) \
	containerof(_req, struct td_xenblkif_req, msg)

#endif /* __TD_REQ_H__ */
