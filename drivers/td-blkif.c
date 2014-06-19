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
tapdisk_xenblkif_show_io_ring_destroy(struct td_xenblkif *blkif)
{
    int err = shm_destroy(&blkif->shm);
    if (err)
        goto out;

    if (blkif->shm.path) {
        char *p = strrchr(blkif->shm.path, '/');
        *p = '\0';
        err = rmdir(blkif->shm.path);
        *p = '/';
        if (err && errno != ENOENT) {
            err = errno;
            EPRINTF("failed to remove %s: %s\n",
                    blkif->shm.path, strerror(err));
            goto out;
        }
        err = 0;
        free(blkif->shm.path);
        blkif->shm.path = NULL;
    }
out:
    return -err;
}


/*
 * TODO Ideally we should provide this information throught the RRD. We would
 * have to modify xen-ringwatch accordingly.
 */
static int
tapdisk_xenblkif_show_io_ring_create(struct td_xenblkif *blkif)
{
    int err, len;
    char *dir = NULL, *_dir = NULL, *_path = NULL;

    len = asprintf(&blkif->shm.path, "/dev/shm/vbd3-%d-%d/io_ring~",
            blkif->domid, blkif->devid);
    if (unlikely(len == -1)) {
        err = errno;
        blkif->shm.path = NULL;
        goto out;
    }

    _dir = strdup(blkif->shm.path);
    if (!_dir) {
        err = errno;
        goto out;
    }
    dir = dirname(_dir);
	err = mkdir(dir, S_IRUSR | S_IWUSR);
	if (err) {
        err = errno;
        if (err != EEXIST) {
            EPRINTF("failed to create %s: %s\n", dir, strerror(err));
    		goto out;
        }
        err = 0;
    }

    blkif->shm.size = PAGE_SIZE;
    blkif->last = 0;

    err = shm_create(&blkif->shm);
    if (err) {
        EPRINTF("failed to create shm ring stats file: %s\n", strerror(err));
        goto out;
    }

    err = tapdisk_xenblkif_show_io_ring(blkif);
    if (unlikely(err)) {
        EPRINTF("failed to generate shared I/O ring stats: %s\n",
                strerror(-err));
        goto out;
    }

    _path = strndup(blkif->shm.path, len - 1);
    if (unlikely(!_path)) {
        err = errno;
        goto out;
    }

    err = rename(blkif->shm.path, _path);
    if (unlikely(err)) {
        err = errno;
        goto out;
    }

    free(blkif->shm.path);
    blkif->shm.path = _path;
    _path = NULL;

out:
    free(_dir);
    free(_path);
    if (err) {
        int err2 = tapdisk_xenblkif_show_io_ring_destroy(blkif);
        if (err2)
            EPRINTF("failed to clean up failed ring stats file: "
                    "%s (error ignored)\n", strerror(-err2));
    }
    return -err;
}


int
tapdisk_xenblkif_destroy(struct td_xenblkif * blkif)
{
    int err;

    ASSERT(blkif);

    tapdisk_xenblkif_reqs_free(blkif);

    if (blkif->ctx) {
        if (blkif->port >= 0)
            xc_evtchn_unbind(blkif->ctx->xce_handle, blkif->port);

        if (blkif->rings.common.sring)
            xc_gnttab_munmap(blkif->ctx->xcg_handle, blkif->rings.common.sring,
                    blkif->ring_n_pages);

		list_del(&blkif->entry_ctx);
        list_del(&blkif->entry);
        tapdisk_xenio_ctx_put(blkif->ctx);
    }

    err = tapdisk_xenblkif_show_io_ring_destroy(blkif);
    if (err)
        EPRINTF("failed to clean up ring stats file: %s (error ignored)\n",
                strerror(-err));

    free(blkif);

    return err;
}


int
tapdisk_xenblkif_disconnect(const domid_t domid, const int devid)
{
    struct td_xenblkif *blkif;

    blkif = tapdisk_xenblkif_find(domid, devid);
    if (!blkif)
        return -ENODEV;

    if (blkif->n_reqs_free != blkif->ring_size) {
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
        /*
         * FIXME shall we unmap the ring or will that lead to some fatal error
         * in tapdisk? IIUC if we don't unmap it we'll get errors during grant
         * copy.
         */
        return 0;
	} else
        return tapdisk_xenblkif_destroy(blkif);
}

int
tapdisk_xenblkif_connect(domid_t domid, int devid, const grant_ref_t * grefs,
        int order, evtchn_port_t port, int proto, const char *pool,
        td_vbd_t * vbd)
{
    struct td_xenblkif *td_blkif = NULL;
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

    shm_init(&td_blkif->shm);

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

    err = tapdisk_xenblkif_show_io_ring_create(td_blkif);
    if (err)
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

event_id_t
tapdisk_xenblkif_event_id(const struct td_xenblkif *blkif)
{
	return blkif->ctx->ring_event;
}

int
tapdisk_xenblkif_show_io_ring(struct td_xenblkif *blkif)
{
    time_t t;
    int err = 0;
	struct blkif_common_back_ring *ring = NULL;

    if (!blkif)
        return 0;

    ring = &blkif->rings.common;
	if (!ring->sring)
        return 0;

    ASSERT(blkif->shm.mem);

    /*
     * Update the ring stats once every thirty seconds.
     */
    t = time(NULL);
	if (t - blkif->last < 30)
		return 0;
	blkif->last = t;

    err = snprintf(blkif->shm.mem, blkif->shm.size,
            "nr_ents %u\n"
            "req prod %u cons %d event %u\n"
            "rsp prod %u pvt %d event %u\n",
            ring->nr_ents,
            ring->sring->req_prod, ring->req_cons, ring->sring->req_event,
            ring->sring->rsp_prod, ring->rsp_prod_pvt, ring->sring->rsp_event);
    if (err < 0)
        err = errno;
    else if (err >= blkif->shm.size)
        err = ENOBUFS;
    else
        err = 0;
    return -err;
}
