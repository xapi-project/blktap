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
 * Process requests on the ring, if any. Returns the number of requests found.
 */
int
tapdisk_xenio_ctx_process_ring(struct td_xenblkif *blkif,
		struct td_xenio_ctx *ctx, int final);

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
#define tapdisk_xenio_ctx_find_blkif(_ctx, _blkif, _cond)	\
	do {													\
		int found = 0;										\
		tapdisk_xenio_for_each_blkif(_blkif, _ctx) {		\
			if (!_blkif->dead && _cond) {                   \
				found = 1;									\
				break;										\
			}												\
		}													\
		if (!found)											\
			_blkif = NULL;									\
	} while (0)

#endif /* __TD_CTX_H__ */
