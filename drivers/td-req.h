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
	container_of(_req, struct td_xenblkif_req, msg)

#endif /* __TD_REQ_H__ */
