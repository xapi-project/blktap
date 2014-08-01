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
 *
 * This file contains the handler executed when the front-end XenStore path of
 * a VBD gets modified.
 */

#include "tapback.h"

#include <xen/io/protocols.h>
#include "xen_blkif.h"

/**
 * Switches the back-end state of the device by writing to XenStore.
 *
 * @param device the VBD
 * @param state the state to switch to
 * @returns 0 on success, an error code otherwise
 */
int
xenbus_switch_state(vbd_t * const device,
        const XenbusState state)
{
    int err;

    ASSERT(device);

    /*
     * TODO Ensure @state contains a legitimate XenbusState value.
     * TODO Check for valid state transitions?
     */

    err = -tapback_device_printf(device, XBT_NULL, "state", false, "%u",
            state);
    if (err)
        WARN(device, "failed to switch back-end state to %s: %s\n",
                xenbus_strstate(state), strerror(err));
    else {
        DBG(device, "switched back-end state to %s\n", xenbus_strstate(state));
        device->state = state;
    }
    return err;
}

/**
 * Core functions that instructs the tapdisk to connect to the shared ring (if
 * not already connected).
 *
 * If the tapdisk is not already connected, all the necessary information is
 * read from XenStore and the tapdisk gets connected using this information.
 * This function is idempotent: if the tapback daemon gets restarted this
 * function will be called again but it won't really do anything.
 *
 * @param device the VBD the tapdisk should connect to
 * @returns (a) 0 on success, (b) ESRCH if the tapdisk is not available, and
 * (c) an error code otherwise
 */
static inline int
connect_tap(vbd_t * const device)
{
    evtchn_port_t port = 0;
    grant_ref_t *gref = NULL;
    int err = 0;
    char *proto_str = NULL;
    char *persistent_grants_str = NULL;
    int nr_pages = 0, proto = 0, order = 0;
    bool persistent_grants = false;

    ASSERT(device);

    /*
     * FIXME disconnect if already connected and then reconnect, this is how
     * blkback does.
     * FIXME If we're already connected, why did we end up here in the first
     * place?
     */
    ASSERT(!device->connected);

    /*
     * The physical-device XenStore key has not been written yet.
     */
    if (!device->tap) {
        DBG(device, "no tapdisk yet\n");
        err = ESRCH;
        goto out;
    }
    /*
     * TODO How can we make sure we're not missing a node written by the
     * front-end? Use xs_directory?
     */

    if (1 != tapback_device_scanf_otherend(device, XBT_NULL, RING_PAGE_ORDER,
                "%d", &order))
        order = 0;

     nr_pages = 1 << order;

    if (!(gref = calloc(nr_pages, sizeof(grant_ref_t)))) {
        WARN(device, "failed to allocate memory for grant refs.\n");
        err = ENOMEM;
        goto out;
    }

    /*
     * Read the grant references.
     */
    if (order) {
        int i = 0;
        /*
         * +10 is for INT_MAX, +1 for NULL termination
         */

        static const size_t len = sizeof(RING_REF) + 10 + 1;
        char ring_ref[len];
        for (i = 0; i < nr_pages; i++) {
            if (snprintf(ring_ref, len, "%s%d", RING_REF, i) >= (int)len) {
                DBG(device, "error printing to buffer\n");
                err = EINVAL;
                goto out;
            }
            if (1 != tapback_device_scanf_otherend(device, XBT_NULL, ring_ref,
                        "%u", &gref[i])) {
                WARN(device, "failed to read grant ref 0x%x\n", i);
                err = ENOENT;
                goto out;
            }
        }
    } else {
        if (1 != tapback_device_scanf_otherend(device, XBT_NULL, RING_REF,
                    "%u", &gref[0])) {
            WARN(device, "failed to read grant ref\n");
            err = ENOENT;
            goto out;
        }
    }

    /*
     * Read the event channel.
     */
    if (1 != tapback_device_scanf_otherend(device, XBT_NULL, EVENT_CHANNEL,
                "%u", &port)) {
        WARN(device, "failed to read event channel\n");
        err = ENOENT;
        goto out;
    }

    /*
     * Read the guest VM's ABI.
     */
    if (!(proto_str = tapback_device_read_otherend(device, XBT_NULL, PROTO)))
        proto = BLKIF_PROTOCOL_X86_32;
    else if (!strcmp(proto_str, XEN_IO_PROTO_ABI_NATIVE))
        proto = BLKIF_PROTOCOL_NATIVE;
    else if (!strcmp(proto_str, XEN_IO_PROTO_ABI_X86_32))
        proto = BLKIF_PROTOCOL_X86_32;
    else if (!strcmp(proto_str, XEN_IO_PROTO_ABI_X86_64))
        proto = BLKIF_PROTOCOL_X86_64;
    else {
        WARN(device, "unsupported protocol %s\n", proto_str);
        err = EINVAL;
        goto out;
    }

    DBG(device, "protocol=%d\n", proto);

    /*
     * Does the front-end support persistent grants?
     */
    persistent_grants_str = tapback_device_read_otherend(device, XBT_NULL,
            FEAT_PERSIST);
    if (persistent_grants_str) {
        if (!strcmp(persistent_grants_str, "0"))
            persistent_grants = false;
        else if (!strcmp(persistent_grants_str, "1"))
            persistent_grants = true;
        else {
            WARN(device, "invalid %s value: %s\n", FEAT_PERSIST,
                    persistent_grants_str);
            err = EINVAL;
            goto out;
        }
    }
    else
        DBG(device, "front-end doesn't support persistent grants\n");

    /*
     * persistent grants are not yet supported
     */
    if (persistent_grants)
        WARN(device, "front-end supports persistent grants but we don't\n");

    /*
     * Create the shared ring and ask the tapdisk to connect to it.
     */
    if ((err = -tap_ctl_connect_xenblkif(device->tap->pid, device->domid,
                    device->devid, gref, order, port, proto, NULL,
                    device->minor))) {
        /*
         * This happens if the tapback dameon gets restarted while there are
         * active VBDs.
         */
        if (err == EALREADY) {
            INFO(device, "tapdisk[%d] minor=%d already connected to the "
					"shared ring\n", device->tap->pid, device->tap->minor);
            err = 0;
        } else {
            WARN(device, "tapdisk[%d] failed to connect to the shared "
                    "ring: %s\n", device->tap->pid, strerror(-err));
            goto out;
        }
    }

    device->connected = true;

    DBG(device, "tapdisk[%d] connected to shared ring\n", device->tap->pid);

out:
    if (err && device->connected) {
        const int err2 = -tap_ctl_disconnect_xenblkif(device->tap->pid,
                device->domid, device->devid, NULL);
        if (err2) {
            WARN(device, "error disconnecting tapdisk[%d] from the shared "
                    "ring (error ignored): %s\n", device->tap->pid,
                    strerror(err2));
        }

        device->connected = false;
    }

    free(gref);
    free(proto_str);
    free(persistent_grants_str);

    return err;
}


/**
 * Returns 0 on success, a negative error code otherwise.
 */
static inline int
connect_frontend(vbd_t *device) {

    int err = 0;
    xs_transaction_t xst = XBT_NULL;
    bool abort_transaction = false;

    ASSERT(device);

    do {
        if (!(xst = xs_transaction_start(device->backend->xs))) {
            err = -errno;
            WARN(device, "failed to start transaction: %s\n", strerror(err));
            goto out;
        }

        abort_transaction = true;

        /*
         * FIXME blkback writes discard-granularity, discard-alignment,
         * discard-secure, feature-discard but we don't.
         */

        /*
		 * Write the number of sectors, sector size, info, and barrier support
		 * to the back-end path in XenStore so that the front-end creates a VBD
		 * with the appropriate characteristics.
         */
        if ((err = tapback_device_printf(device, xst, "feature-barrier", true,
                        "%d", 1))) {
            WARN(device, "failed to write feature-barrier: %s\n",
					strerror(-err));
            break;
        }

        if ((err = tapback_device_printf(device, xst, "sector-size", true,
                        "%u", device->sector_size))) {
            WARN(device, "failed to write sector-size: %s\n", strerror(-err));
            break;
        }

        if ((err = tapback_device_printf(device, xst, "sectors", true, "%llu",
                        device->sectors))) {
            WARN(device, "failed to write sectors: %s\n", strerror(-err));
            break;
        }

        if ((err = tapback_device_printf(device, xst, "info", true, "%u",
                        device->info))) {
            WARN(device, "failed to write info: %s\n", strerror(-err));
            break;
        }

		abort_transaction = false;
        if (!xs_transaction_end(device->backend->xs, xst, 0)) {
            err = -errno;
            ASSERT(err);
        }
    } while (err == -EAGAIN);

    if (abort_transaction) {
        if (!xs_transaction_end(device->backend->xs, xst, 1)) {
            int err2 = errno;
            WARN(device, "failed to abort transaction: %s\n", strerror(err2));
        }
        goto out;
    }

    if (err) {
        WARN(device, "failed to end transaction: %s\n", strerror(-err));
        goto out;
    }

    err = -xenbus_switch_state(device, XenbusStateConnected);
    if (err)
        WARN(device, "failed to switch back-end state to connected: %s\n",
                strerror(-err));
out:
    return err;
}

/*
 * Returns 0 on success, a positive error code otherwise.
 *
 * If tapdisk is not yet available (the physical-device key has not yet been
 * written), ESRCH is returned.
 */
static inline int
xenbus_connect(vbd_t *device) {
    int err;

    ASSERT(device);

    err = connect_tap(device);
    /*
     * No tapdisk yet (the physical-device XenStore key has not been written).
     */
    if (err == ESRCH)
        goto out;
    /*
     * Even if tapdisk is already connected to the shared ring, we continue
     * connecting since we don't know how far the connection process had gone
     * before the tapback daemon was restarted.
     */
    if (err && err != -EALREADY)
        goto out;
    err = -connect_frontend(device);
out:
    return err;
}

/**
 * Callback that is executed when the front-end goes to StateClosed.
 *
 * Instructs the tapdisk to disconnect itself from the shared ring and switches
 * the back-end state to StateClosed.
 *
 * @param xdevice the VBD whose tapdisk should be disconnected
 * @param state unused
 * @returns 0 on success, a +errno otherwise
 *
 * XXX Only called by frontend_changed.
 */
static inline int
backend_close(vbd_t * const device)
{
    int err = 0;

    ASSERT(device);

    if (!device->connected) {
        /*
         * This VBD might be a CD-ROM device, or a disk device that never went
         * to state Connected.
         */
		if (device->tap)
	        DBG(device, "tapdisk[%d] not connected\n", device->tap->pid);
		else
	        DBG(device, "no tapdisk connected\n");
    } else {
        ASSERT(device->tap);

        DBG(device, "disconnecting tapdisk[%d] minor=%d from the ring\n",
            device->tap->pid, device->minor);

		err = -tap_ctl_disconnect_xenblkif(device->tap->pid, device->domid,
				device->devid, NULL);
		if (err) {
			if (err == ESRCH) {/* tapdisk might have died :-( */
				WARN(device, "tapdisk[%d] not running\n", device->tap->pid);
				err = 0;
			} else {
				WARN(device, "error disconnecting tapdisk[%d] minor=%d from "
						"the ring: %s\n", device->tap->pid, device->minor,
						strerror(err));
				return err;
			}
		}

        device->connected = false;
    }

    return xenbus_switch_state(device, XenbusStateClosed);
}

int
frontend_changed(vbd_t * const device, const XenbusState state)
{
    int err = 0;

    DBG(device, "front-end switched to state %s\n", xenbus_strstate(state));
	device->frontend_state = state;

    switch (state) {
        case XenbusStateInitialising:
			if (device->hotplug_status_connected)
				err = xenbus_switch_state(device, XenbusStateInitWait);
            break;
        case XenbusStateInitialised:
    	case XenbusStateConnected:
            if (!device->hotplug_status_connected)
                DBG(device, "udev scripts haven't yet run\n");
            else {
                if (device->state != XenbusStateConnected) {
                    DBG(device, "connecting to front-end\n");
                    err = xenbus_connect(device);
                } else
                    DBG(device, "already connected\n");
            }
            break;
        case XenbusStateClosing:
            err = xenbus_switch_state(device, XenbusStateClosing);
            break;
        case XenbusStateClosed:
            err = backend_close(device);
            break;
        case XenbusStateUnknown:
            err = 0;
            break;
        default:
            err = EINVAL;
            WARN(device, "invalid front-end state %d\n", state);
            break;
    }
    return err;
}

int
tapback_backend_handle_otherend_watch(backend_t *backend,
		const char * const path)
{
    vbd_t *device = NULL;
    int err = 0, state = 0;
    char *s = NULL, *end = NULL, *_path = NULL;

	ASSERT(backend);
    ASSERT(path);

    /*
     * Find the device that has the same front-end state path.
     *
     * There should definitely be such a device in our list, otherwise this
     * function would not have executed at all, since we would not be waiting
     * on that XenStore path.  The XenStore path we wait for is:
     * /local/domain/<domid>/device/vbd/<devname>/state. In order to watch this
     * path, it means that we have received a device create request, so the
     * device will be there.
     *
     * TODO Instead of this linear search we could do better (hash table etc).
     */
    tapback_backend_find_device(backend, device,
            device->frontend_state_path &&
			!strcmp(device->frontend_state_path, path));
    if (!device) {
        WARN(NULL, "path \'%s\' does not correspond to a known device\n",
                path);
        return ENODEV;
    }

    /*
     * Read the new front-end's state.
     */
	s = tapback_xs_read(device->backend->xs, XBT_NULL, "%s",
			device->frontend_state_path);
    if (!s) {
        err = errno;
		/*
         * If the front-end XenBus node is missing, the XenBus device has been
         * removed: remove the XenBus back-end node.
		 */
		if (err == ENOENT) {
            err = asprintf(&_path, "%s/%s/%d/%d", XENSTORE_BACKEND,
                    device->backend->name, device->domid, device->devid);
            if (err == -1) {
                err = errno;
                WARN(device, "failed to asprintf: %s\n", strerror(err));
                goto out;
            }
            err = 0;
            if (!xs_rm(device->backend->xs, XBT_NULL, _path)) {
                if (errno != ENOENT) {
                    err = errno;
                    WARN(device, "failed to remove %s: %s\n", path,
                            strerror(err));
                }
            }
		}
    } else {
        state = strtol(s, &end, 0);
        if (*end != 0 || end == s) {
            WARN(device, "invalid XenBus state '%s'\n", s);
            err = EINVAL;
        } else
            err = frontend_changed(device, state);
    }

out:
    free(s);
    free(_path);
    return err;
}

struct backend_slave*
tapback_find_slave(const backend_t *master, const domid_t domid) {

    struct backend_slave _slave, **__slave = NULL;

    ASSERT(master);

    _slave.master.domid = domid;

    __slave = tfind(&_slave, &master->master.slaves, compare);
    if (!__slave)
        return NULL;
    return *__slave;
}
