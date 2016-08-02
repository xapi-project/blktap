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

#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <xenctrl.h>
#include <unistd.h>
#include <libgen.h>
#include <zlib.h>

#include "debug.h"
#include "blktap3.h"
#include "tapdisk.h"
#include "tapdisk-log.h"
#include "util.h"
#include "tapdisk-server.h"
#include "tapdisk-metrics.h"
#include "timeout-math.h"

#include "td-blkif.h"
#include "td-ctx.h"
#include "td-req.h"

struct td_xenblkif *
tapdisk_xenblkif_find(const domid_t domid, const int devid)
{
    struct td_xenblkif *blkif = NULL;
    struct td_xenio_ctx *ctx;

    tapdisk_xenio_for_each_ctx(ctx) {
        tapdisk_xenio_ctx_find_blkif(ctx, blkif,
                                     blkif->domid == domid &&
                                     blkif->devid == devid);
        if (blkif)
            return blkif;
    }

    return NULL;
}


/**
 * Returns 0 on success, -errno on failure.
 */
static int
tapdisk_xenblkif_stats_destroy(struct td_xenblkif *blkif)
{
    int err;

    err = shm_destroy(&blkif->xenvbd_stats.io_ring);
    if (unlikely(err))
        goto out;
    free(blkif->xenvbd_stats.io_ring.path);
    blkif->xenvbd_stats.io_ring.path = NULL;

    err = shm_destroy(&blkif->xenvbd_stats.stats);
    if (unlikely(err))
        goto out;
    free(blkif->xenvbd_stats.stats.path);
    blkif->xenvbd_stats.stats.path = NULL;

    if (likely(blkif->xenvbd_stats.root)) {
        err = rmdir(blkif->xenvbd_stats.root);
        if (unlikely(err && errno != ENOENT)) {
            err = errno;
            EPRINTF("failed to remove %s: %s\n",
                    blkif->xenvbd_stats.root, strerror(err));
            goto out;
        }
        err = 0;

        free(blkif->xenvbd_stats.root);
        blkif->xenvbd_stats.root = NULL;
    }
out:
    return -err;
}


/*
 * TODO provide ring stats in raw format (the same way I/O stats are provided).
 * xen-ringwatch will have to be modified accordingly.
 */
static int
tapdisk_xenblkif_stats_create(struct td_xenblkif *blkif)
{
    int err = 0, len;
    char *_path = NULL;

    len = asprintf(&blkif->xenvbd_stats.root, "/dev/shm/vbd3-%d-%d",
            blkif->domid, blkif->devid);
    if (unlikely(len == -1)) {
        err = errno;
        blkif->xenvbd_stats.root = NULL;
        goto out;
    }

	err = mkdir(blkif->xenvbd_stats.root, S_IRUSR | S_IWUSR);
	if (unlikely(err)) {
        err = errno;
        if (err != EEXIST) {
            EPRINTF("failed to create %s: %s\n",
                    blkif->xenvbd_stats.root, strerror(err));
    		goto out;
        }
        err = 0;
    }

    len = asprintf(&blkif->xenvbd_stats.io_ring.path, "%s/io_ring~",
            blkif->xenvbd_stats.root);
    if (unlikely(len == -1)) {
        err = errno;
        blkif->xenvbd_stats.io_ring.path = NULL;
        goto out;
    }
    blkif->xenvbd_stats.io_ring.size = PAGE_SIZE;
    err = shm_create(&blkif->xenvbd_stats.io_ring);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to create shm ring stats file: %s\n", strerror(err));
        goto out;
   }

    err = asprintf(&blkif->xenvbd_stats.stats.path, "%s/statistics",
            blkif->xenvbd_stats.root);
    if (unlikely(err == -1)) {
        err = errno;
        blkif->xenvbd_stats.stats.path = NULL;
        goto out;
    }
    blkif->xenvbd_stats.stats.size = PAGE_SIZE;
    err = shm_create(&blkif->xenvbd_stats.stats);
    if (unlikely(err))
        goto out;

    blkif->xenvbd_stats.last = 0;

	blkif->stats.xenvbd = blkif->xenvbd_stats.stats.mem;

    if (tapdisk_server_mem_mode()) {
        td_flag_set(blkif->stats.xenvbd->flags, BT3_LOW_MEMORY_MODE);
        td_flag_set(blkif->vbd_stats.stats->flags, BT3_LOW_MEMORY_MODE);
    }

    err = tapdisk_xenblkif_ring_stats_update(blkif);
    if (unlikely(err)) {
        EPRINTF("failed to generate shared I/O ring stats: %s\n",
                strerror(-err));
        goto out;
    }

    _path = strndup(blkif->xenvbd_stats.io_ring.path, len - 1);
    if (unlikely(!_path)) {
        err = errno;
        goto out;
    }

    err = rename(blkif->xenvbd_stats.io_ring.path, _path);
    if (unlikely(err)) {
        err = errno;
        goto out;
    }

    free(blkif->xenvbd_stats.io_ring.path);
    blkif->xenvbd_stats.io_ring.path = _path;
    _path = NULL;
out:
    free(_path);
    if (err) {
        int err2 = tapdisk_xenblkif_stats_destroy(blkif);
        if (err2)
            EPRINTF("failed to clean up failed stats file: "
                    "%s (error ignored)\n", strerror(-err2));
    }
    return -err;
}


int
tapdisk_xenblkif_destroy(struct td_xenblkif * blkif)
{
    int err;

    ASSERT(blkif);

    if (tapdisk_xenblkif_chkrng_event_id(blkif) >= 0) {
        tapdisk_server_unregister_event(
				tapdisk_xenblkif_chkrng_event_id(blkif));
        blkif->chkrng_event = -1;
    }

    if (tapdisk_xenblkif_stoppolling_event_id(blkif) >= 0) {
        tapdisk_server_unregister_event(
				tapdisk_xenblkif_stoppolling_event_id(blkif));
        blkif->stoppolling_event = -1;
    }

    tapdisk_xenblkif_reqs_free(blkif);

    if (blkif->ctx) {
        if (blkif->port >= 0)
            xc_evtchn_unbind(blkif->ctx->xce_handle, blkif->port);

        if (blkif->rings.common.sring) {
            err = xc_gnttab_munmap(blkif->ctx->xcg_handle,
					blkif->rings.common.sring, blkif->ring_n_pages);
			if (unlikely(err)) {
				err = errno;
				EPRINTF("failed to unmap ring page %p (%d pages): %s "
						"(error ignored)\n",
						blkif->rings.common.sring, blkif->ring_n_pages,
						strerror(err));
				err = 0;
			}
		}

		list_del(&blkif->entry_ctx);
        list_del(&blkif->entry);
        tapdisk_xenio_ctx_put(blkif->ctx);
    }
    err = td_metrics_vbd_stop(&blkif->vbd_stats);
    if (unlikely(err))
        EPRINTF("failed to destroy blkfront stats file: %s\n", strerror(-err));

    err = tapdisk_xenblkif_stats_destroy(blkif);
    if (unlikely(err)) {
        EPRINTF("failed to clean up ring stats file: %s (error ignored)\n",
                strerror(-err));
        err = 0;
    }

    free(blkif);

    return err;
}


int
tapdisk_xenblkif_reqs_pending(const struct td_xenblkif * const blkif)
{
	ASSERT(blkif);

	return blkif->ring_size - blkif->n_reqs_free;
}


int
tapdisk_xenblkif_disconnect(const domid_t domid, const int devid)
{
    int err;
    struct td_xenblkif *blkif;

    blkif = tapdisk_xenblkif_find(domid, devid);
    if (!blkif)
        return -ENODEV;

    if (tapdisk_xenblkif_reqs_pending(blkif)) {
        RING_DEBUG(blkif, "disconnect from ring with %d pending requests\n",
                blkif->ring_size - blkif->n_reqs_free);
		if (td_flag_test(blkif->vbd->state, TD_VBD_PAUSED))
			RING_ERR(blkif, "disconnect from ring with %d pending requests "
                    "and the VBD paused\n",
                    blkif->ring_size - blkif->n_reqs_free);
        list_move(&blkif->entry, &blkif->vbd->dead_rings);
        blkif->dead = true;
        if (blkif->ctx && blkif->port >= 0) {
            xc_evtchn_unbind(blkif->ctx->xce_handle, blkif->port);
            blkif->port = -1;
        }

        err = td_metrics_vbd_stop(&blkif->vbd_stats);
        if (unlikely(err))
            EPRINTF("failed to destroy blkfront stats file: %s\n", strerror(-err));

        err = tapdisk_xenblkif_stats_destroy(blkif);
        if (unlikely(err))
            EPRINTF("failed to clean up ring stats file: %s (error ignored)\n",
                    strerror(-err));

        /*
         * FIXME shall we unmap the ring or will that lead to some fatal error
         * in tapdisk? IIUC if we don't unmap it we'll get errors during grant
         * copy.
         */
        return 0;
	} else
        return tapdisk_xenblkif_destroy(blkif);
}


void
tapdisk_xenblkif_sched_stoppolling(const struct td_xenblkif *blkif)
{
	int err;

	ASSERT(blkif);

	err = tapdisk_server_event_set_timeout(
		tapdisk_xenblkif_stoppolling_event_id(blkif), TV_USECS(blkif->poll_duration));
	ASSERT(!err);
}

void
tapdisk_xenblkif_unsched_stoppolling(const struct td_xenblkif *blkif)
{
	int err;

	ASSERT(blkif);

	err = tapdisk_server_event_set_timeout(
		tapdisk_xenblkif_stoppolling_event_id(blkif), TV_INF);
	ASSERT(!err);
}


void
tapdisk_start_polling(struct td_xenblkif *blkif)
{
    ASSERT(blkif);

    /* Only enter polling if the CPU utilisation is not too high */
    if (tapdisk_server_system_idle_cpu() > (float)blkif->poll_idle_threshold) {
        blkif->in_polling = true;

        /* Start checking the ring immediately */
        tapdisk_xenblkif_sched_chkrng(blkif);

        /* Schedule the future 'stop polling' event */
        tapdisk_xenblkif_sched_stoppolling(blkif);
    }
}

static inline void
tapdisk_xenblkif_cb_stoppolling(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_xenblkif *blkif = private;

    ASSERT(blkif);

    /* Process the ring one final time, setting the event counter */
    if (!tapdisk_xenio_ctx_process_ring(blkif, blkif->ctx, 1)) {
        /* If there were no new requests this time, then stop polling */
        blkif->in_polling = false;

        /* Stop obsessively checking the ring */
        tapdisk_xenblkif_unsched_chkrng(blkif);

        /* Make the 'stop polling' event not fire again */
        tapdisk_xenblkif_unsched_stoppolling(blkif);
    }
}

void
tapdisk_xenblkif_sched_chkrng(const struct td_xenblkif *blkif)
{
	int err;

	ASSERT(blkif);

	err = tapdisk_server_event_set_timeout(
			tapdisk_xenblkif_chkrng_event_id(blkif), TV_ZERO);
	ASSERT(!err);
}

void
tapdisk_xenblkif_unsched_chkrng(const struct td_xenblkif *blkif)
{
	int err;

	ASSERT(blkif);

	err = tapdisk_server_event_set_timeout(
			tapdisk_xenblkif_chkrng_event_id(blkif), TV_INF);
	ASSERT(!err);
}

static inline void
tapdisk_xenblkif_cb_chkrng(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_xenblkif *blkif = private;

    ASSERT(blkif);

    /*
     * If we are polling, process the ring without setting the event counter.
     * If we are not polling, unschedule this event, process the ring and set
     * the event counter.
     */

    if (!blkif->in_polling)
        tapdisk_xenblkif_unsched_chkrng(blkif);

    tapdisk_xenio_ctx_process_ring(blkif, blkif->ctx, !blkif->in_polling);
}


int
tapdisk_xenblkif_connect(domid_t domid, int devid, const grant_ref_t * grefs,
        int order, evtchn_port_t port, int proto, int poll_duration,
        int poll_idle_threshold, const char *pool, td_vbd_t * vbd)
{
    struct td_xenblkif *td_blkif = NULL; /* TODO rename to blkif */
    struct td_xenio_ctx *td_ctx;
    int err;
    unsigned int i;
    void *sring;
    size_t sz;

    ASSERT(grefs);
    ASSERT(vbd);

    /*
     * Already connected?
     */
    if (tapdisk_xenblkif_find(domid, devid)) {
        /* TODO log error */
        return -EALREADY;
    }

    err = tapdisk_xenio_ctx_get(pool, &td_ctx);
    if (err) {
        /* TODO log error */
        goto fail;
    }

    td_blkif = calloc(1, sizeof(*td_blkif));
    if (!td_blkif) {
        /* TODO log error */
        err = -errno;
        goto fail;
    }

    td_blkif->domid = domid;
    td_blkif->devid = devid;
    td_blkif->vbd = vbd;
    td_blkif->ctx = td_ctx;
    td_blkif->proto = proto;
    td_blkif->dead = false;
	td_blkif->chkrng_event = -1;
	td_blkif->stoppolling_event = -1;
	td_blkif->in_polling = false;
	td_blkif->poll_duration = poll_duration;
	td_blkif->poll_idle_threshold = poll_idle_threshold;
	td_blkif->barrier.msg = NULL;
	td_blkif->barrier.io_done = false;
	td_blkif->barrier.io_err = 0;

    td_blkif->xenvbd_stats.root = NULL;
    shm_init(&td_blkif->xenvbd_stats.io_ring);
    shm_init(&td_blkif->xenvbd_stats.stats);

    memset(&td_blkif->stats, 0, sizeof(td_blkif->stats));

    INIT_LIST_HEAD(&td_blkif->entry_ctx);
    INIT_LIST_HEAD(&td_blkif->entry);

    /*
     * Create the shared ring.
     */
    td_blkif->ring_n_pages = 1 << order;
    if (td_blkif->ring_n_pages > ARRAY_SIZE(td_blkif->ring_ref)) {
        RING_ERR(td_blkif, "too many pages (%u), max %zu\n",
                td_blkif->ring_n_pages, ARRAY_SIZE(td_blkif->ring_ref));
        err = -EINVAL;
        goto fail;
    }

    /*
     * TODO Why don't we just keep a copy of the array's address? There should
     * be a reason for copying the addresses of the pages, figure out why.
     * TODO Why do we even store it in the td_blkif in the first place?
     */
    for (i = 0; i < td_blkif->ring_n_pages; i++)
        td_blkif->ring_ref[i] = grefs[i];

    /*
     * Map the grant references that will be holding the request descriptors.
     */
    sring = xc_gnttab_map_domain_grant_refs(td_blkif->ctx->xcg_handle,
            td_blkif->ring_n_pages, td_blkif->domid, td_blkif->ring_ref,
            PROT_READ | PROT_WRITE);
    if (!sring) {
        err = -errno;
        RING_ERR(td_blkif, "failed to map domain's grant references: %s\n",
                strerror(-err));
        goto fail;
    }

    /*
     * Size of the ring, in bytes.
     */
    sz = XC_PAGE_SIZE << order;

    /*
     * Initialize the mapped address into the shared ring.
     *
     * TODO Check for protocol support in the beginning of this function.
     */
    switch (td_blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            {
                blkif_sring_t *__sring = sring;
                BACK_RING_INIT(&td_blkif->rings.native, __sring, sz);
                break;
            }
        case BLKIF_PROTOCOL_X86_32:
            {
                blkif_x86_32_sring_t *__sring = sring;
                BACK_RING_INIT(&td_blkif->rings.x86_32, __sring, sz);
                break;
            }
        case BLKIF_PROTOCOL_X86_64:
            {
                blkif_x86_64_sring_t *__sring = sring;
                BACK_RING_INIT(&td_blkif->rings.x86_64, __sring, sz);
                break;
            }
        default:
            RING_ERR(td_blkif, "unsupported protocol 0x%x\n", td_blkif->proto);
            err = -EPROTONOSUPPORT;
            goto fail;
    }

    /*
     * Bind to the remote port.
     * TODO elaborate
     */
    td_blkif->port = xc_evtchn_bind_interdomain(td_blkif->ctx->xce_handle,
            td_blkif->domid, port);
    if (td_blkif->port == -1) {
        err = -errno;
        RING_ERR(td_blkif, "failed to bind to event channel port %d: %s\n",
                port, strerror(-err));
        goto fail;
    }

    err = tapdisk_xenblkif_reqs_init(td_blkif);
    if (err) {
        /* TODO log error */
        goto fail;
    }

	td_blkif->chkrng_event = tapdisk_server_register_event(
			SCHEDULER_POLL_TIMEOUT,	-1, TV_INF,
			tapdisk_xenblkif_cb_chkrng, td_blkif);
    if (unlikely(td_blkif->chkrng_event < 0)) {
        err = td_blkif->chkrng_event;
        RING_ERR(td_blkif, "failed to register event: %s\n", strerror(-err));
        goto fail;
    }

    err = td_metrics_vbd_start(td_blkif->domid, td_blkif->devid, &td_blkif->vbd_stats);
    if (unlikely(err))
        goto fail;

	td_blkif->stoppolling_event = tapdisk_server_register_event(
			SCHEDULER_POLL_TIMEOUT,	-1, TV_INF,
			tapdisk_xenblkif_cb_stoppolling, td_blkif);
    if (unlikely(td_blkif->stoppolling_event < 0)) {
        err = td_blkif->stoppolling_event;
        RING_ERR(td_blkif, "failed to register event: %s\n", strerror(-err));
        goto fail;
    }

    err = tapdisk_xenblkif_stats_create(td_blkif);
    if (unlikely(err))
        goto fail;

    list_add_tail(&td_blkif->entry, &vbd->rings);
	list_add_tail(&td_blkif->entry_ctx, &td_ctx->blkifs);

    DPRINTF("ring %p connected\n", td_blkif);

    return 0;

fail:
    if (td_blkif) {
        int err2 = tapdisk_xenblkif_destroy(td_blkif);
        if (err2)
            EPRINTF("failed to destroy the block interface: %s "
                    "(error ignored)\n", strerror(-err2));
    }

    return err;
}


inline event_id_t
tapdisk_xenblkif_evtchn_event_id(const struct td_xenblkif *blkif)
{
	return blkif->ctx->ring_event;
}


inline event_id_t
tapdisk_xenblkif_chkrng_event_id(const struct td_xenblkif *blkif)
{
	return blkif->chkrng_event;
}


inline event_id_t
tapdisk_xenblkif_stoppolling_event_id(const struct td_xenblkif *blkif)
{
	return blkif->stoppolling_event;
}


int
tapdisk_xenblkif_ring_stats_update(struct td_xenblkif *blkif)
{
    time_t t;
    int err = 0, len;
	struct blkif_common_back_ring *ring = NULL;
	uLong *chksum = NULL;

    if (!blkif)
        return 0;

    if (unlikely(blkif->dead))
        return 0;

    ring = &blkif->rings.common;
	if (!ring->sring)
        return 0;

    ASSERT(blkif->xenvbd_stats.io_ring.mem);

    /*
     * Update the ring stats once every five seconds.
     */
    t = time(NULL);
	if (t - blkif->xenvbd_stats.last < 5)
		return 0;
	blkif->xenvbd_stats.last = t;

    len = snprintf(blkif->xenvbd_stats.io_ring.mem + sizeof(*chksum),
            blkif->xenvbd_stats.io_ring.size,
            "nr_ents %u\n"
            "req prod %u cons %d event %u\n"
            "rsp prod %u pvt %d event %u\n",
            ring->nr_ents,
            ring->sring->req_prod, ring->req_cons, ring->sring->req_event,
            ring->sring->rsp_prod, ring->rsp_prod_pvt, ring->sring->rsp_event);
    if (unlikely(len < 0))
        err = errno;
    else if (unlikely(len + sizeof(uLong) >= blkif->xenvbd_stats.io_ring.size))
        err = ENOBUFS;
    else {
        err = ftruncate(blkif->xenvbd_stats.io_ring.fd, len + sizeof(*chksum));
        if (unlikely(err)) {
            err = errno;
            EPRINTF("failed to truncate %s to %lu: %s\n",
                    blkif->xenvbd_stats.io_ring.path, len + sizeof(*chksum),
					strerror(err));
        }
    }

	chksum = blkif->xenvbd_stats.io_ring.mem;
	*chksum = crc32(0L, blkif->xenvbd_stats.io_ring.mem + sizeof(*chksum),
			len);

    return -err;
}


void
tapdisk_xenblkif_suspend(struct td_xenblkif * const blkif)
{
	ASSERT(blkif);

	tapdisk_server_mask_event(tapdisk_xenblkif_evtchn_event_id(blkif), 1);
	tapdisk_server_mask_event(tapdisk_xenblkif_chkrng_event_id(blkif), 1);
}


void
tapdisk_xenblkif_resume(struct td_xenblkif * const blkif)
{
	ASSERT(blkif);

	tapdisk_server_mask_event(tapdisk_xenblkif_evtchn_event_id(blkif), 0);
	tapdisk_server_mask_event(tapdisk_xenblkif_chkrng_event_id(blkif), 0);
}


bool
tapdisk_xenblkif_barrier_should_complete(
		const struct td_xenblkif * const blkif)
{
	ASSERT(blkif);

	return blkif->barrier.msg && 1 == tapdisk_xenblkif_reqs_pending(blkif) &&
		(0 == blkif->barrier.msg->nr_segments || blkif->barrier.io_done);
}
