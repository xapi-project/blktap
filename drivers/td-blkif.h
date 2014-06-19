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

    struct list_head entry_dead;

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

    struct shm shm;
    time_t last;

    /**
     * Request buffer cache.
     */
    void **reqs_bufcache;
    unsigned n_reqs_bufcache_free;
    event_id_t reqs_bufcache_evtid;
};

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
 * @param pool name of the context
 * @param vbd the VBD
 * @returns 0 on success
 */
int
tapdisk_xenblkif_connect(domid_t domid, int devid, const grant_ref_t * grefs,
        int order, evtchn_port_t port, int proto, const char *pool,
        td_vbd_t * vbd);

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

event_id_t
tapdisk_xenblkif_event_id(const struct td_xenblkif *blkif);

int
tapdisk_xenblkif_show_io_ring(struct td_xenblkif *blkif);

bool
tapdisk_xenblkif_is_dead(const struct td_xenblkif * const blkif);

#endif /* __TD_BLKIF_H__ */
