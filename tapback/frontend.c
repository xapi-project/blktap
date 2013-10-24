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
static int
tapback_device_switch_state(vbd_t * const device,
        const XenbusState state)
{
    int err;

    ASSERT(device);

    /*
     * TODO Ensure @state contains a legitimate XenbusState value.
     * TODO Check for valid state transitions?
     */

    err = -tapback_device_printf(device, "state", false, "%u", state);
    if (err) {
        WARN("failed to switch back-end state to %s: %s\n",
                XenbusState2str(state), strerror(err));
    } else
        DBG("switched back-end state to %s\n", XenbusState2str(state));
    return err;
}

/**
 * Core functions that instructs the tapdisk to connect to the shared ring (if
 * not already connected) and communicates essential information to the
 * front-end.
 *
 * If the tapdisk is not already connected, all the necessary information is
 * read from XenStore and the tapdisk gets connected using this information.
 *
 * TODO Why should this function be called on an already connected VBD? Why
 * re-write the sector size etc. in XenStore for an already connected VBD?
 * TODO rename function (no blkback, not only connects to tapdisk)
 *
 * @param bdev the VBD the tapdisk should connect to
 * @param state unused
 * @returns 0 on success, an error code otherwise
 *
 * XXX Only called by blkback_frontend_changed, when the front-end switches to
 * Initialised and Connected.
 */
static int
blkback_connect_tap(vbd_t * const bdev,
        const XenbusState state __attribute__((unused)))
{
    evtchn_port_t port = 0;
    grant_ref_t *gref = NULL;
    int err = 0;
    char *proto_str = NULL;
    char *persistent_grants_str = NULL;

    ASSERT(bdev);

    if (bdev->connected) {
        /*
         * TODO Fail with EALREADY?
         */
        DBG("front-end already connected to tapdisk.\n");

        /*
         * FIXME just testing
         */
        if ((err = tapback_device_switch_state(bdev,
                        XenbusStateConnected))) {
            WARN("failed to switch back-end state to connected: %s\n",
                    strerror(err));
        }
    } else if (!bdev->tap) {
        DBG("no tapdisk yet\n");
        err = EAGAIN;
    } else {
        /*
         * TODO How can we make sure we're not missing a node written by the
         * front-end? Use xs_directory?
         */
        int nr_pages = 0, proto = 0, order = 0;
        bool persistent_grants = false;
    	bool do_connect = true;

        if (1 != tapback_device_scanf_otherend(bdev, "ring-page-order", "%d",
                    &order))
            order = 0;

         nr_pages = 1 << order;

        if (!(gref = calloc(nr_pages, sizeof(grant_ref_t)))) {
            WARN("Failed to allocate memory for grant refs.\n");
            err = ENOMEM;
            goto fail;
        }

        /*
         * Read the grant references.
         */
        if (order) {
            int i = 0;
            /*
             * +10 is for INT_MAX, +1 for NULL termination
             */

            /*
             * TODO include domid/devid in the error messages
             */
            static const size_t len = sizeof(RING_REF) + 10 + 1;
            char ring_ref[len];
            for (i = 0; i < nr_pages; i++) {
                if (snprintf(ring_ref, len, "%s%d", RING_REF, i) >= (int)len) {
                    DBG("error printing to buffer\n");
                    err = EINVAL;
                    goto fail;
                }
                if (1 != tapback_device_scanf_otherend(bdev, ring_ref, "%u",
                            &gref[i])) {
                    WARN("Failed to read grant ref 0x%x.\n", i);
                    err = ENOENT;
                    goto fail;
                }
            }
        } else {
            if (1 != tapback_device_scanf_otherend(bdev, RING_REF, "%u",
                        &gref[0])) {
                WARN("Failed to read grant ref.\n");
                err = ENOENT;
                goto fail;
            }
        }

        /*
         * Read the event channel.
         */
        if (1 != tapback_device_scanf_otherend(bdev, "event-channel", "%u",
                    &port)) {
            WARN("Failed to read event channel.\n");
            err = ENOENT;
            goto fail;
        }

        /*
         * Read the guest VM's ABI.
         */
        if (!(proto_str = tapback_device_read_otherend(bdev, "protocol")))
            proto = BLKIF_PROTOCOL_NATIVE;
        else if (!strcmp(proto_str, XEN_IO_PROTO_ABI_X86_32))
            proto = BLKIF_PROTOCOL_X86_32;
        else if (!strcmp(proto_str, XEN_IO_PROTO_ABI_X86_64))
            proto = BLKIF_PROTOCOL_X86_64;
        else {
            WARN("unsupported protocol %s\n", proto_str);
            err = EINVAL;
            goto fail;
        }

        /*
         * Does the front-end support persistent grants?
         */
        persistent_grants_str = tapback_device_read_otherend(bdev,
                FEAT_PERSIST);
        if (persistent_grants_str) {
            if (!strcmp(persistent_grants_str, "0"))
                persistent_grants = false;
            else if (!strcmp(persistent_grants_str, "1"))
                persistent_grants = true;
            else {
                WARN("invalid %s value: %s\n", FEAT_PERSIST,
                        persistent_grants_str);
                err = EINVAL;
                goto fail;
            }
        }
        else
            DBG("front-end doesn't support persistent grants\n");

        /*
         * persistent grants are not yet supported
         */
        if (persistent_grants)
            WARN("front-end supports persistent grants but we don't\n");

        /*
         * Create the shared ring and ask the tapdisk to connect to it.
		 *
		 * FIXME write sectors, sector size etc. to xenstore before connecting?
         */
		if ((err = -tap_ctl_connect_xenblkif(bdev->tap->pid, bdev->domid,
						bdev->devid, gref, order, port, proto, NULL,
						bdev->minor))) {
			if (err == EALREADY) {
				INFO("%d/%d: tapdisk[%d] already connected to the shared "
						"ring\n", bdev->domid, bdev->devid, bdev->tap->pid);
				do_connect = false;
				err = 0;
			} else {
	            WARN("%d/%d: tapdisk[%d] failed to connect to the shared "
						"ring: %s\n", bdev->domid, bdev->devid, bdev->tap->pid,
		                strerror(-err));
			    goto fail;
			}
        } else {
	        DBG("%d/%d: tapdisk[%d] connected to shared ring\n",
		            bdev->domid, bdev->devid, bdev->tap->pid);
		}

        bdev->connected = true;

        if (do_connect) {
            /*
             * Write the number of sectors, sector size, and info to the
             * back-end path in XenStore so that the front-end creates a VBD
             * with the appropriate characteristics.
             */
            if ((err = tapback_device_printf(bdev, "sector-size", true, "%u",
                            bdev->sector_size))) {
                WARN("Failed to write sector-size.\n");
                goto fail;
            }

            if ((err = tapback_device_printf(bdev, "sectors", true, "%llu",
                            bdev->sectors))) {
                WARN("Failed to write sectors.\n");
                goto fail;
            }

            if ((err = tapback_device_printf(bdev, "info", true, "%u",
                            bdev->info))) {
                WARN("Failed to write info.\n");
                goto fail;
            }

            if ((err = tapback_device_switch_state(bdev,
                            XenbusStateConnected))) {
                WARN("failed to switch back-end state to connected: %s\n",
                        strerror(err));
            }
        }
	}

fail:
    if (err && bdev->connected) {
        const int err2 = -tap_ctl_disconnect_xenblkif(bdev->tap->pid,
                bdev->domid, bdev->devid, NULL);
        if (err2) {
            WARN("%d/%d: error disconnecting tapdisk[%d] from the shared "
                    "ring (error ignored): %s\n", bdev->domid, bdev->devid,
                    bdev->tap->pid, strerror(err2));
        }

        bdev->connected = false;
    }

    free(gref);
    free(proto_str);
    free(persistent_grants_str);

    return err;
}

/**
 * Removes the Xenbus back-end node.
 */
static int
xenbus_rm_backend(vbd_t * const bdev) {

	int err = 0;
	char *path = NULL;
	bool result = false;

	ASSERT(bdev);

	err = asprintf(&path, "%s/%s/%d/%d", XENSTORE_BACKEND,
			BLKTAP3_BACKEND_NAME, bdev->domid, bdev->devid);
	if (err == -1) {
		err = errno;
		WARN("failed to asprintf for %d/%d: %s\n", bdev->domid,
				bdev->devid, strerror(err));
		return err;
	}
	err = 0;
	result = xs_rm(blktap3_daemon.xs, blktap3_daemon.xst, path);
	if (!result) {
		err = errno;
		WARN("failed to remove %s for %d/%d: %s\n", path, bdev->domid,
				bdev->devid, strerror(err));
	}
	free(path);
	return err;
}

/**
 * Callback that is executed when the front-end goes to StateClosed.
 *
 * Instructs the tapdisk to disconnect itself from the shared ring and switches
 * the back-end state to StateClosed.
 *
 * @param xbdev the VBD whose tapdisk should be disconnected
 * @param state unused
 * @returns 0 on success, a positive error code otherwise
 *
 * XXX Only called by blkback_frontend_changed.
 */
static inline int
backend_close(vbd_t * const bdev,
		const XenbusState state __attribute__((unused)))
{
    int err = 0;

    ASSERT(bdev);

    if (!bdev->connected) {
        /*
         * This VBD might be a CD-ROM device, or a disk device that never went
         * to state Connected.
         */
        DBG("tapdisk not connected\n");
    } else {
        ASSERT(bdev->tap);

        DBG("%d/%d: disconnecting from tapdisk[%d] minor=%d\n",
            bdev->domid, bdev->devid, bdev->tap->pid, bdev->minor);

        if ((err = -tap_ctl_disconnect_xenblkif(bdev->tap->pid, bdev->domid,
                        bdev->devid, NULL))) {

            /*
             * TODO I don't see how tap_ctl_disconnect_xenblkif can return
             * ESRCH, so this is probably wrong. Probably there's another error
             * code indicating that there's no tapdisk process.
             */
            if (errno == ESRCH) {
                WARN("tapdisk not running\n");
            } else {
                WARN("error disconnecting tapdisk from front-end: %s\n",
                        strerror(err));
                return err;
            }
        }

        bdev->connected = false;
    }

    return tapback_device_switch_state(bdev, XenbusStateClosed);
}

/**
 * Acts on changes in the front-end state.
 *
 * TODO The back-end blindly follows the front-ends state transitions, should
 * we check whether unexpected transitions are performed?
 *
 * @param xbdev the VBD whose front-end state changed
 * @param state the new state
 * @returns 0 on success, an error code otherwise
 *
 * XXX Only called by tapback_device_check_front-end_state.
 */
static inline int
blkback_frontend_changed(vbd_t * const xbdev, const XenbusState state)
{
    /*
     * XXX The size of the array (9) comes from the XenbusState enum.
     *
     * TODO Send a patch that adds XenbusStateMin, XenbusStateMax,
     * XenbusStateInvalid and in the XenbusState enum (located in xenbus.h).
     *
     * The front-end's state is used as the array index. Each element contains
     * a call-back function to be executed in response, and an optional state
     * for the back-end to switch to.
     */
    static struct frontend_state_change {
        int (*fn)(vbd_t * const, const XenbusState);
        const XenbusState state;
    } const frontend_state_change_map[] = {
        [XenbusStateUnknown] = {NULL, 0},
        [XenbusStateInitialising]
            = {tapback_device_switch_state, XenbusStateInitWait},
        [XenbusStateInitWait] = {NULL, 0},

        /* blkback_connect_tap switches back-end state to Connected */
        [XenbusStateInitialised] = {blkback_connect_tap, 0},
        [XenbusStateConnected] = {blkback_connect_tap, 0},

        [XenbusStateClosing]
            = {tapback_device_switch_state, XenbusStateClosing},
        [XenbusStateClosed] = {backend_close, 0},
        [XenbusStateReconfiguring] = {NULL, 0},
        [XenbusStateReconfigured] = {NULL, 0}
    };

    ASSERT(xbdev);
    ASSERT(state <= XenbusStateReconfigured);

    DBG("front-end %d/%s went into state %s\n",
            xbdev->domid, xbdev->name, XenbusState2str(state));

    if (frontend_state_change_map[state].fn)
        return frontend_state_change_map[state].fn(xbdev,
                frontend_state_change_map[state].state);
    else
        DBG("ignoring front-end's %d/%s transition to state %s\n",
                xbdev->domid, xbdev->name, XenbusState2str(state));
    return 0;
}

int
tapback_backend_handle_otherend_watch(const char * const path)
{
    vbd_t *device = NULL;
    int err = 0, state = 0;
    char *s = NULL, *end = NULL;

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
    tapback_backend_find_device(device,
            device->frontend_state_path &&
			!strcmp(device->frontend_state_path, path));
    if (!device) {
        WARN("path \'%s\' does not correspond to a known device\n", path);
        return ENODEV;
    }

    DBG("device: %d/%s\n", device->domid, device->name);

    /*
     * Read the new front-end's state.
     */
	s = tapback_xs_read(blktap3_daemon.xs, blktap3_daemon.xst, "%s",
			device->frontend_state_path);
    if (!s) {
        err = errno;
		/*
         * Remove the back-end.
		 */
		if (err == ENOENT) {
			err = xenbus_rm_backend(device);
			if (err && err != ENOENT) {
				WARN("%d/%s: failed to remove the back-end node: %s\n",
						device->domid, device->name, strerror(err));
			} else {
				err = 0;
				goto out;
			}
		}
        goto out;
    }
    state = strtol(s, &end, 0);
    if (*end != 0 || end == s) {
        err = EINVAL;
        goto out;
    }

    err = blkback_frontend_changed(device, state);

out:
    free(s);
    return err;
}
