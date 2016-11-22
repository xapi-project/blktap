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

#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <xenctrl.h>
#include <unistd.h>
#include <libgen.h>

#include "debug.h"
#include "blktap3.h"
#include "tapdisk.h"
#include "tapdisk-log.h"
#include "util.h"
#include "tapdisk-server.h"

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

    EPRINTF("I/O stats create started for VBD with domid %d and devid %d\n",
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
tapdisk_xenblkif_sched_chkrng(const struct td_xenblkif *blkif)
{
	int err;

	ASSERT(blkif);

	err = tapdisk_server_event_set_timeout(
			tapdisk_xenblkif_chkrng_event_id(blkif), 0);
	ASSERT(!err);
}


static inline void
tapdisk_xenblkif_cb_chkrng(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_xenblkif *blkif = private;
	int err;

    ASSERT(blkif);

	err = tapdisk_server_event_set_timeout(
			tapdisk_xenblkif_chkrng_event_id(blkif), (time_t) - 1);
	ASSERT(!err);

    tapdisk_xenio_ctx_process_ring(blkif, blkif->ctx, 1);
}


int
tapdisk_xenblkif_connect(domid_t domid, int devid, const grant_ref_t * grefs,
        int order, evtchn_port_t port, int proto, const char *pool,
        td_vbd_t * vbd)
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
			SCHEDULER_POLL_TIMEOUT,	-1, (time_t) - 1,
			tapdisk_xenblkif_cb_chkrng, td_blkif);
    if (unlikely(td_blkif->chkrng_event < 0)) {
        err = td_blkif->chkrng_event;
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


int
tapdisk_xenblkif_ring_stats_update(struct td_xenblkif *blkif)
{
    time_t t;
    int err = 0, len;
	struct blkif_common_back_ring *ring = NULL;

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

    len = snprintf(blkif->xenvbd_stats.io_ring.mem,
            blkif->xenvbd_stats.io_ring.size,
            "nr_ents %u\n"
            "req prod %u cons %d event %u\n"
            "rsp prod %u pvt %d event %u\n",
            ring->nr_ents,
            ring->sring->req_prod, ring->req_cons, ring->sring->req_event,
            ring->sring->rsp_prod, ring->rsp_prod_pvt, ring->sring->rsp_event);
    if (unlikely(len < 0))
        err = errno;
    else if (unlikely(len >= blkif->xenvbd_stats.io_ring.size))
        err = ENOBUFS;
    else {
        err = ftruncate(blkif->xenvbd_stats.io_ring.fd, len);
        if (unlikely(err)) {
            err = errno;
            EPRINTF("failed to truncate %s to %d: %s\n",
                    blkif->xenvbd_stats.io_ring.path, len, strerror(err));
        }
    }

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
