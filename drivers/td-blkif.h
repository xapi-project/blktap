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

#ifndef __TD_BLKIF_H__
#define __TD_BLKIF_H__

#include <inttypes.h> /* required by xen/event_channel.h */

#include <xen/xen.h>
#include <xen/io/xenbus.h>
#include <xen/event_channel.h>
#include <xen/grant_table.h>
#include <stdbool.h>

#include "xen_blkif.h"
#include "td-req.h"
#include "td-stats.h"
#include "tapdisk-vbd.h"
#include "tapdisk-utils.h"
#include "tapdisk-metrics.h"

struct td_xenio_ctx;
struct td_vbd_handle;
struct td_xenblkif_stats;

struct td_xenblkif {

    /**
     * The domain ID where the front-end is running.
     */
    int domid;

    /**
     * The device ID of the VBD.
     */
    int devid;


    /**
	 * Pointer to the context this block interface belongs to.
	 */
    struct td_xenio_ctx *ctx;

    /**
	 * allows struct td_blkif's to be linked into lists, for whomever needs to
	 * maintain multiple struct td_blkif's
	 */
    struct list_head entry_ctx;

    struct list_head entry;

    /**
     * The local port corresponding to the remote port of the domain where the
     * front-end is running. We use this to tell for which VBD a pending event
     * is, and for notifying the front-end for responses we have produced and
     * placed in the shared ring.
     */
	/*
	 * FIXME shoud be evtchn_port_or_error_t, which is declared in
	 * xenctrl.h. Including xenctrl.h conflicts with xen_blkif.h.
	 */
     int port;

    /**
     * protocol (native, x86, or x64)
     * Need to keep around? Replace with function pointer?
     */
    int proto;

    blkif_back_rings_t rings;

    /**
     * TODO Why 8 specifically?
     * TODO Do we really need to keep it around?
     */
    grant_ref_t ring_ref[8];

    /**
     * Number of pages in the ring that holds the request descriptors.
     */
    unsigned int ring_n_pages;

    /*
     * Size of the ring, expressed in requests.
     * TODO Do we really need to keep this around?
     */
    int ring_size;

    /**
     * Intermediate requests. The array is managed as a stack, with n_reqs_free
     * pointing to the top of the stack, at the next available intermediate
     * request.
     */
    struct td_xenblkif_req *reqs;

    /**
     * Stack pointer to the aforementioned stack.
     */
    int n_reqs_free;

    blkif_request_t **reqs_free;

    /**
     * Pointer to the actual VBD.
     */
    struct td_vbd_handle *vbd;

    /**
     * stats
     */
    struct td_xenblkif_stats stats;

    stats_t vbd_stats;

    struct {
        /**
         * Root directory of the stats.
         */
        char *root;

        /**
         * Xenbus ring
         */
        struct shm io_ring;

        /**
         * blkback-style stats. We keep all seven of them in a single file
         * because keeping each one in a separate file requires an entire
         * page because of mmap(2). The order is: ds_req, f_req, oo_req,
         * rd_req, rd_sect, wr_req, and wr_sect.
         */
        struct shm stats;

        time_t last;
    } xenvbd_stats;

    /**
     * Request buffer cache.
     */
    void **reqs_bufcache;
    unsigned n_reqs_bufcache_free;
    event_id_t reqs_bufcache_evtid;

	bool dead;

	struct {
		/**
		 * Pointer to he pending barrier request.
		 */
		blkif_request_t *msg;

		/**
		 * Tells whether the write I/O part of a barrier request (if any) has
		 * completed.
		 */
		bool io_done;

		/**
		 * I/O error code for the write I/O part of a barrier request (if any).
		 */
		int io_err;
	} barrier;

	event_id_t chkrng_event;
	event_id_t stoppolling_event;

	bool in_polling;
	int poll_duration; /* microseconds; 0 means no polling. */
	int poll_idle_threshold;
};

#define RING_DEBUG(blkif, fmt, args...)                                     \
    DPRINTF("%d/%d, ring=%p: "fmt, (blkif)->domid, (blkif)->devid, (blkif), \
        ##args);

#define RING_ERR(blkif, fmt, args...)                                       \
    EPRINTF("%d/%d, ring=%p: "fmt, (blkif)->domid, (blkif)->devid, (blkif), \
        ##args);

/* TODO rename from xenio */
#define tapdisk_xenio_for_each_ctx(_ctx) \
	list_for_each_entry(_ctx, &_td_xenio_ctxs, entry)

/**
 * Connects the tapdisk to the shared ring.
 *
 * @param domid the ID of the guest domain
 * @param devid the device ID
 * @param grefs the grant references
 * @param order number of grant references
 * @param port event channel port of the guest domain to use for ring
 * notifications
 * @param proto protocol (native, x86, or x64)
 * @param poll_duration polling duration (microseconds; 0 means no polling)
 * @param poll_idle_threshold CPU threshold above which we permit polling
 * @param pool name of the context
 * @param vbd the VBD
 * @returns 0 on success
 */
int
tapdisk_xenblkif_connect(domid_t domid, int devid, const grant_ref_t * grefs,
        int order, evtchn_port_t port, int proto, int poll_duration,
        int poll_idle_threshold, const char *pool, td_vbd_t * vbd);

/**
 * Disconnects the tapdisk from the shared ring.
 *
 * @param domid the domain ID of the guest domain
 * @param devid the device ID of the VBD
 *
 * @returns 0 on success, or one of the following error codes:
 * -ENODEV: no such domain and/or device
 * -EBUSY: there are pending requests in the ring
 * -ESHUTDOWN: there are pending requests and the VBD is paused
 */
int
tapdisk_xenblkif_disconnect(const domid_t domid, const int devid);

/**
 * Destroys a XEN block interface.
 *
 * @param blkif the block interface to destroy
 */
int
tapdisk_xenblkif_destroy(struct td_xenblkif * blkif);

/**
 * Searches all block interfaces in all contexts for a block interface
 * having the specified domain and device ID. Dead block interfaces are
 * ignored.
 *
 * @param domid the domain ID
 * @param devid the device ID
 * @returns a pointer to the block interface if found, else NULL
 */
struct td_xenblkif *
tapdisk_xenblkif_find(const domid_t domid, const int devid);

/**
 * Returns the event ID associated with the event channel. Since the event
 * channel can be shared by multiple block interfaces, the event ID will be
 * shared as well.
 */
extern inline event_id_t
tapdisk_xenblkif_evtchn_event_id(const struct td_xenblkif *blkif);

/**
 * Returns the event ID associated wit checking the ring. This is a private
 * event.
 */
extern inline event_id_t
tapdisk_xenblkif_chkrng_event_id(const struct td_xenblkif * const blkif);

/**
 * Returns the event ID associated with stopping polling. This is a private
 * event.
 */
extern inline event_id_t
tapdisk_xenblkif_stoppolling_event_id(const struct td_xenblkif * const blkif);

/**
 * Updates ring stats.
 */
int
tapdisk_xenblkif_ring_stats_update(struct td_xenblkif *blkif);

/**
 * Suspends the operation of the ring. NB the operation of the ring might
 * have been already suspended.
 */
void
tapdisk_xenblkif_suspend(struct td_xenblkif * const blkif);

/**
 * Resumes the operation of the ring.
 */
void
tapdisk_xenblkif_resume(struct td_xenblkif * const blkif);

/**
 * Tells how many requests are pending.
 */
int
tapdisk_xenblkif_reqs_pending(const struct td_xenblkif * const blkif);

/**
 * Schedules the cessation of polling.
 */
void
tapdisk_xenblkif_sched_stoppolling(const struct td_xenblkif *blkif);

/**
 * Unschedules the cessation of polling.
 */
void
tapdisk_xenblkif_unsched_stoppolling(const struct td_xenblkif *blkif);

/**
 * Start polling now.
 */
void
tapdisk_start_polling(struct td_xenblkif *blkif);

/**
 * Schedules a ring check.
 */
void
tapdisk_xenblkif_sched_chkrng(const struct td_xenblkif *blkif);

/**
 * Unschedules the ring check.
 */
void
tapdisk_xenblkif_unsched_chkrng(const struct td_xenblkif *blkif);

/**
 * Tells whether a barrier request can be completed.
 */
bool
tapdisk_xenblkif_barrier_should_complete(
		const struct td_xenblkif * const blkif);

#endif /* __TD_BLKIF_H__ */
