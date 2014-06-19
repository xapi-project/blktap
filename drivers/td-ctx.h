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

#ifndef __TD_CTX_H__
#define __TD_CTX_H__

#include <xenctrl.h>
#include "td-blkif.h"
#include "scheduler.h"

/**
 * A VBD context: groups two or more VBDs of the same tapdisk.
 *
 * TODO The purpose of this struct is dubious: it allows one or more VBDs
 * belonging to the same tapdisk process to share the same handle to the event
 * channel driver. This means that when an event triggers on some event
 * channel, this notification will be delivered via the file descriptor of the
 * handle. Thus, we need to look which exactly event channel triggered. This
 * functionality is a trade off between reducing the total amount of open
 * handles to the event channel driver versus speeding up the data path. Also,
 * it effectively allows for more event channels to be polled by select. The
 * bottom line is that if we use one VBD per tapdisk this functionality is
 * unnecessary.
 */
struct td_xenio_ctx {
    char *pool; /* TODO rename to pool_name */

    /**
     * Handle to the grant table driver.
     */
    xc_gnttab *xcg_handle;

    /**
     * Handle to the event channel driver.
     */
    xc_evtchn *xce_handle;

    /**
     * Return value of tapdisk_server_register_event, we use this to tell
     * whether the context is registered.
     */
    event_id_t ring_event;

    /**
     * block interfaces in this pool
     */
    struct list_head blkifs;

    /**
     * Allow struct td_xenio_ctx to be part of a linked list.
     */
    struct list_head entry;

    int gntdev_fd;
};

/**
 * Retrieves the context corresponding to the specified pool name, creating it
 * if it doesn't already exist.
 *
 * @returns 0 on success, -errno on error
 */
int
tapdisk_xenio_ctx_get(const char *pool, struct td_xenio_ctx ** _ctx);

/**
 * Releases the pool, only if there is no block interface using it.
 */
void
tapdisk_xenio_ctx_put(struct td_xenio_ctx * ctx);

/**
 * Process requests on the ring, if any.
 */
void
tapdisk_xenio_ctx_process_ring(struct td_xenblkif *blkif,
		               struct td_xenio_ctx *ctx);

/**
 * List of contexts.
 */
extern struct list_head _td_xenio_ctxs;

/**
 * For each block interface of this context...
 */
#define tapdisk_xenio_for_each_blkif(_blkif, _ctx)	\
	list_for_each_entry(_blkif, &(_ctx)->blkifs, entry_ctx)

/**
 * Search this context for the block interface for which the condition is true.
 * Dead block interfaces are ignored.
 */
#define tapdisk_xenio_ctx_find_blkif(_ctx, _blkif, _cond)	    \
	do {													    \
		int found = 0;										    \
		tapdisk_xenio_for_each_blkif(_blkif, _ctx) {		    \
			if (!tapdisk_xenblkif_is_dead(_blkif) && _cond) {   \
				found = 1;									    \
				break;										    \
			}												    \
		}													    \
		if (!found)											    \
			_blkif = NULL;									    \
	} while (0)

#endif /* __TD_CTX_H__ */
