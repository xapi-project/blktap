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
 * This file contains the handler executed when the back-end XenStore path gets
 * modified.
 */

#include "config.h"

#include "tapback.h"
#include "xenstore.h"
#include <xen/io/blkif.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

/**
 * Removes the XenStore watch from the front-end.
 *
 * @param device the VBD whose front-end XenStore path should stop being
 * watched
 */
static void
tapback_device_unwatch_frontend_state(vbd_t * const device)
{
    ASSERT(device);

    if (device->frontend_state_path)
		/* TODO check return code */
        xs_unwatch(device->backend->xs, device->frontend_state_path,
                device->backend->frontend_token);

    free(device->frontend_state_path);
    device->frontend_state_path = NULL;
}

/**
 * Destroys and deallocates the back-end part of a VBD.
 *
 * @param device the VBD to destroy
 * @returns 0 on success, -errno on failure.
 */
static int
tapback_backend_destroy_device(vbd_t * const device)
{
	int err;

    ASSERT(device);

    DBG(device, "removing VBD\n");

	if (device->tap && device->connected) {

		DBG(device, "implicitly disconnecting tapdisk[%d] minor=%d from the "
				"ring\n", device->tap->pid, device->minor);

		err = tap_ctl_disconnect_xenblkif(device->tap->pid, device->domid,
				device->devid, NULL);
		if (err) {
			if (err == -ESRCH || err == -ENOENT) {
				/*
				 * TODO tapdisk might have died without cleaning up, in which
				 * case we'll receive an I/O error when trying to talk to it
				 * through the socket, maybe search for a process with that
				 * PID? Alternatively, we can spawn tapdisks through a daemon
				 * which will monitor tapdisks for abrupt deaths and clean up
				 * after them (e.g. remove the socket).
				 */
				INFO(device, "tapdisk[%d] not running\n", device->tap->pid);
				err = 0;
			} else {
				WARN(device, "cannot disconnect tapdisk[%d] minor=%d from the "
						"ring: %s\n", device->tap->pid, device->minor,
						strerror(-err));
			}
			return err;
		}
	}

    list_del(&device->backend_entry);

    tapback_device_unwatch_frontend_state(device);

	free(device->tap);
    free(device->frontend_path);
    free(device->name);
    free(device);

	return 0;
}

/**
 * Retrieves the tapdisk designated to serve this device, storing this
 * information in the supplied VBD handle.
 *
 * @param minor
 * @param tap output parameter that receives the tapdisk process information.
 * The parameter is undefined when the function returns a non-zero value.
 * @returns 0 if a suitable tapdisk is found, ESRCH if no suitable tapdisk is
 * found, and a negative error code in case of error
 *
 * TODO rename function
 *
 * XXX Only called by blkback_probe.
 */
static inline int
find_tapdisk(const int minor, tap_list_t *tap)
{
    LIST_HEAD(list);
    tap_list_t *_tap;
    int err;

    err = tap_ctl_list(&list);
    if (err) {
        WARN(NULL, "error listing tapdisks: %s\n", strerror(-err));
        goto out;
    }

    err = -ESRCH;
    if (!list_empty(&list)) {
        tap_list_for_each_entry(_tap, &list) {
            if (_tap->minor == minor) {
                err = 0;
                memcpy(tap, _tap, sizeof(*tap));
                break;
            }
        }
        tap_ctl_list_free(&list);
    } else
        DBG(NULL, "no tapdisks\n");

out:
    return err;
}

/**
 * Creates a device and adds it to the list of devices.
 *
 * Creating the device implies initializing the handle and retrieving all the
 * information of the tapdisk serving this VBD.
 *
 * @param domid the ID of the domain where the VBD is created
 * @param name the name of the device
 * @returns the created device, NULL on error, sets errno
 *
 * FIXME If the transaction fails with any error code other than EAGAIN, we're
 * not undoing everything we did in this function.
 */
static inline vbd_t*
tapback_backend_create_device(backend_t *backend,
		const domid_t domid, const char * const name)
{
    vbd_t *device = NULL;
    int err = 0;

	ASSERT(backend);
    ASSERT(name);

    DBG(NULL, "%s creating device\n", name);

    if (!(device = calloc(1, sizeof(*device)))) {
        WARN(NULL, "error allocating memory\n");
        err = errno;
        goto out;
    }

	device->backend = backend;
    list_add_tail(&device->backend_entry,
            &device->backend->slave.slave.devices);

    device->major = -1;
    device->minor = -1;

	device->mode = false;
	device->cdrom = false;
	device->info = 0;

    /* TODO check for errors */
    device->devid = atoi(name);

    device->domid = domid;

	device->state = XenbusStateUnknown;
	device->frontend_state = XenbusStateUnknown;

    if (!(device->name = strdup(name))) {
        err = -errno;
        goto out;
    }

out:
    if (err) {
        WARN(NULL, "%s: error creating device: %s\n", name, strerror(-err));
        if (device) {
            int err2 = tapback_backend_destroy_device(device);
			if (err2)
				WARN(device, "error cleaning up: failed to destroy the "
						"device: %s\n", strerror(-err2));
		}
        device = NULL;
        errno = err;
    }
    return device;
}

static int
device_set_mode(vbd_t *device, const char *mode) {

	ASSERT(device);
	ASSERT(mode);

	if (!strcmp(mode, "r"))
		device->mode = false;
	else if (!strcmp(mode, "w"))
		device->mode = true;
	else {
		WARN(device, "invalid value %s for XenStore key %s\n",
				mode, MODE_KEY);
		return -EINVAL;
	}

	return 0;
}

/**
 * Retrieves the minor number and locates the corresponding tapdisk, storing
 * all relevant information in @device. Then, it attempts to advance the Xenbus
 * state as it might be that everything is ready and all that was missing was
 * the physical device.
 *
 * We might already have read the major/minor, but if the physical-device
 * key triggered we need to make sure it hasn't changed. This also protects us
 * against restarted transactions.
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int
physical_device_changed(vbd_t *device) {

    char * s = NULL, *p = NULL, *end = NULL;
    int err = 0, major = 0, minor = 0;
    unsigned int info;

    ASSERT(device);

    /*
     * Get the minor.
     */
    s = tapback_device_read(device, XBT_NULL, PHYS_DEV_KEY);
    if (!s) {
        err = -errno;
        if (err != -ENOENT)
            WARN(device, "failed to read the physical-device: %s\n",
                    strerror(-err));
        goto out;
    }

    /*
     * The XenStore key physical-device contains "major:minor" in hex.
     */
    p = strtok(s, ":");
    if (!p) {
        WARN(device, "malformed physical device '%s'\n", s);
        err = -EINVAL;
        goto out;
    }
    major = strtol(p, &end, 16);
    if (*end != 0 || end == p) {
        WARN(device, "malformed physical device '%s'\n", s);
        err = -EINVAL;
        goto out;
    }
    p = strtok(NULL, ":");
	if (unlikely(!p)) {
        WARN(device, "malformed physical device '%s'\n", s);
        err = -EINVAL;
        goto out;
	}
    minor = strtol(p, &end, 16);
    if (*end != 0 || end == p) {
        WARN(device, "malformed physical device '%s'\n", s);
        err = -EINVAL;
        goto out;
    }

    if ((device->major >= 0 || device->minor >= 0) &&
            (major != device->major || minor != device->minor)) {
        WARN(device, "changing physical device from %x:%x to %x:%x not "
                "supported\n", device->major, device->minor, major, minor);
        err = -ENOSYS;
        goto out;
    }

    device->major = major;
    device->minor = minor;

    device->tap = malloc(sizeof(*device->tap));
    if (!device->tap) {
        err = -ENOMEM;
        goto out;
    }

    /*
     * XXX If the physical-device key has been written we expect a tapdisk to
     * have been created. If tapdisk is created after the physical-device key
     * is written we have no way of being notified, so we will not be able to
     * advance the back-end state.
     */

    DBG(device, "need to find tapdisk serving minor=%d\n", device->minor);

    err = find_tapdisk(device->minor, device->tap);
    if (err) {
        WARN(device, "error looking for tapdisk: %s\n", strerror(-err));
        goto out;
    }

    DBG(device, "found tapdisk[%d]\n", device->tap->pid);

    /*
     * get the VBD parameters from the tapdisk
     */
    if ((err = tap_ctl_info(device->tap->pid, &device->sectors,
                    &device->sector_size, &info,
                    device->minor))) {
        WARN(device, "error retrieving disk characteristics: %s\n",
                strerror(-err));
        goto out;
    }

	err = tapback_device_printf(device, XBT_NULL, "kthread-pid", false, "%d",
		device->tap->pid);
	if (unlikely(err)) {
		WARN(device, "warning: failed to write kthread-pid: %s\n",
				strerror(-err));
		goto out;
	}

    if (device->sector_size & 0x1ff || device->sectors <= 0) {
        WARN(device, "warning: unexpected device characteristics: sector "
                "size=%d, sectors=%llu\n", device->sector_size,
				device->sectors);
    }

    /*
     * The front-end might have switched to state Connected before
     * physical-device is written. Check it's state and connect if necessary.
     *
     * TODO blkback ignores connection errors, let's do the same until we
     * know better.
     */
    err = -frontend_changed(device, device->frontend_state);
    if (err)
        WARN(device, "failed to switch state: %s (error ignored)\n",
                strerror(-err));
    err = 0;
out:
    if (err) {
        free(device->tap);
        device->tap = NULL;
        device->sector_size = device->sectors = device->info = 0;
    }
    free(s);
    return err;
}

/**
 * Start watching the front-end state path.
 */
static int
frontend(vbd_t *device) {

    int err = 0;

    ASSERT(device);

    if (device->frontend_path)
        return 0;

    /*
     * Get the front-end path from XenStore. We need this to talk to
     * the front-end.
     */
    if (!(device->frontend_path = tapback_device_read(device, XBT_NULL,
                    FRONTEND_KEY))) {
        err = -errno;
        if (err != -ENOENT)
            WARN(device, "failed to read front-end path: %s\n", strerror(-err));
        goto out;
    }

    /*
     * Watch the front-end path in XenStore for changes, i.e.
     * /local/domain/<domid>/device/vbd/<devname>/state After this, we
     * wait for the front-end to switch state to continue with the
     * initialisation.
     */
    if (asprintf(&device->frontend_state_path, "%s/state",
                device->frontend_path) == -1) {
        err = -errno;
        WARN(device, "failed to asprintf: %s\n", strerror(-err));
        goto out;
    }

    /*
     * We use the same token for all front-end watches. We don't have
     * to use a unique token for each front-end watch because when a
     * front-end watch fires we are given the XenStore path that
     * changed.
     */
    if (!xs_watch(device->backend->xs, device->frontend_state_path,
                device->backend->frontend_token)) {
        err = -errno;
        WARN(device, "failed to watch the front-end path (%s): %s\n",
                device->frontend_state_path, strerror(-err));
        goto out;
    }

out:
    if (err) {
        free(device->frontend_path);
        device->frontend_path = NULL;
        free(device->frontend_state_path);
        device->frontend_state_path = NULL;
    }
    return err;
}

/**
 * Reads the hotplug-status XenStore key from the back-end and attemps to
 * connect to the front-end.
 *
 * FIXME This function REQUIRES the device->frontend_path member to be
 * populated, and this is done by frontend().
 *
 * Connecting to the front-end requires the physical-device key to have been
 * written. This function will attempt to connect anyway, and connecting will
 * fail half-way through. This is expected.
 *
 * Returns 0 in success, -errno on failure.
 */
static int
hotplug_status_changed(vbd_t * const device) {

	int err = 0;
	char *hotplug_status = NULL, *device_type = NULL, *mode = NULL;

	ASSERT(device);

	if (unlikely(!device->frontend_path)) {
		/*
		 * Something has gone horribly wrong.
		 */
		WARN(device, "hotplug scripts ran but front-end does not exist\n");
		err = -EINVAL;
		goto out;
	}

	hotplug_status = tapback_device_read(device, XBT_NULL, HOTPLUG_STATUS_KEY);
	if (!hotplug_status) {
		err = -errno;
		goto out;
	}
	if (!strcmp(hotplug_status, "connected")) {

        DBG(device, "physical device available (hotplug scripts ran)\n");

		device->hotplug_status_connected = true;

        device_type = tapback_device_read_otherend(device, XBT_NULL,
                "device-type");
        if (!device_type) {
            err = -errno;
            WARN(device, "failed to read device-type: %s\n", strerror(-err));
            goto out;
        }
        if (!strcmp(device_type, "cdrom"))
            device->cdrom = true;
        else if (!strcmp(device_type, "disk"))
            device->cdrom = false;
        else {
            WARN(device, "unsupported device type %s\n", device_type);
            err = -EOPNOTSUPP;
            goto out;
        }

        mode = tapback_device_read(device, XBT_NULL, MODE_KEY);
        if (!mode) {
            err = -errno;
            WARN(device, "failed to read XenStore key %s: %s\n",
                    MODE_KEY, strerror(-err));
            goto out;
        }
        err = device_set_mode(device, mode);
        if (err) {
            WARN(device, "failed to set device R/W mode: %s\n",
                    strerror(-err));
            goto out;
        }

        if (device->cdrom)
            device->info |= VDISK_CDROM;
        if (!device->mode)
            device->info |= VDISK_READONLY;

        /*
         * Attempt to connect as everything may be ready and the only thing the
         * back-end is waiting for is this XenStore key to be written.
         */
        err = frontend_changed(device, device->frontend_state);
        if (err) {
            /*
             * Ignore connection errors as the front-end might not yet be
             * ready. blkback doesn't wait for this XenStore key to be written,
             * so we choose to handle this the same way we do with
             * physical-device.
             */
            INFO(device, "failed to connect to the front-end: %s "
                    "(error ignored)\n", strerror(err));
        }
        err = 0;
	}
	else {
		WARN(device, "invalid hotplug-status value %s\n", hotplug_status);
		err = -EINVAL;
	}

out:
	free(hotplug_status);
    free(device_type);
	free(mode);

	return err;
}

/**
 * Attempts to reconnected the back-end to the fornt-end if possible (e.g.
 * after a tapback restart), or after the slave tapback has started.
 *
 * The order we call the functions is not important, apart from
 * tapback_backend_handle_otherend_watch that MUST be at the end, because each
 * function attemps to reconnect but won't do so because the front-end state
 * won't have been read.
 *
 * returns 0 on success, an error code otherwise
 *
 * NB that 0 is also returned when a reconnection is not yet feasible
 */
static inline int
reconnect(vbd_t *device) {

    int err;

    DBG(device, "attempting reconnect\n");

    /*
     * frontend() must be called before all other functions.
     */
    err = frontend(device);
    if (err) {
        /*
         * tapdisk or the front-end state path are not available.
         */
        if (err == -ENOENT) {
            DBG(device, "front-end XenStore sub-tree does not yet exist\n");
            err = 0;
        } else if (err == -ESRCH) {
            DBG(device, "tapdisk not yet available\n");
            err = 0;
        } else
            WARN(device, "failed to watch front-end path: %s\n",
                    strerror(-err));
        goto out;
    }

    err = physical_device_changed(device);
    if (err) {
        if (err == -ENOENT) {
            DBG(device, "no physical device yet\n");
            err = 0;
        } else {
            WARN(device, "failed to retrieve physical device information: "
                    "%s\n", strerror(-err));
            goto out;
        }
    }

    err = hotplug_status_changed(device);
    if (err) {
        if (err == -ENOENT) {
            DBG(device, "udev scripts haven't yet run\n");
            err = 0;
        } else {
            WARN(device, "failed to retrieve hotplug information: %s\n",
                    strerror(-err));
            goto out;
        }
    }

    err = -tapback_backend_handle_otherend_watch(device->backend,
			device->frontend_state_path);
    if (err) {
        if (err == -ENOENT) {
            DBG(device, "front-end not yet ready\n");
            err = 0;
        } else
            WARN(device, "error running the Xenbus protocol: %s\n",
                    strerror(-err));
    }
out:
    return err;
}

/**
 * Creates (removes) a device depending on the existence (non-existence) of the
 * "backend/<backend name>/@domid/@devname" XenStore path.
 *
 * @param domid the ID of the domain where the VBD is created
 * @param devname device name
 * @param comp the XenStore component after the
 * backend/<backend name>/@domid/@devname/. Might be NULL if the device just
 * got created.
 * @returns 0 on success, a negative error code otherwise
 */
static int
tapback_backend_probe_device(backend_t *backend,
		const domid_t domid, const char * const devname, const char *comp)
{
    bool should_exist = false, create = false, remove = false;
    int err = 0;
    vbd_t *device = NULL;
    char * s = NULL;

	ASSERT(backend);
    ASSERT(devname);

    ASSERT(!tapback_is_master(backend));

    DBG(NULL, "%s probing device\n", devname);

    /*
     * Ask XenStore if the device _should_ exist.
     */
    s = tapback_xs_read(backend->xs, XBT_NULL, "%s/%s",
            backend->path, devname);
    should_exist = s != NULL;
    free(s);

    /*
     * Search the device list for this specific device.
     */
    tapback_backend_find_device(backend, device,
            device->domid == domid && !strcmp(device->name, devname));

    /*
     * If XenStore says that the device should exist but it's not in our device
     * list, we must create it. If it's the other way round, this is a removal.
     */
    remove = device && !should_exist;
    create = !device && should_exist;

    /*
     * Remember that remove and create may both be true at the same time, as
     * this indicates that the device has been removed and re-created too fast.
     * (FIXME Is this really true?) In this case, we too need to remove and
     * re-create the device, respectively.
     */

    if (remove) {
        err = tapback_backend_destroy_device(device);
		if (err) {
			WARN(device, "failed to destroy the device: %s\n", strerror(-err));
			return err;
		}
		device = NULL;
	}

    if (create) {
        device = tapback_backend_create_device(backend, domid, devname);
        if (!device) {
            err = errno;
            WARN(NULL, "%s error creating device: %s\n", devname,
                    strerror(err));
            return err;
        }

        err = reconnect(device);
        if (err)
            WARN(device, "failed to reconnect: %s\n", strerror(-err));
    }

    /*
     * Examine what happened to the XenStore component on which the watch
     * triggered.
     *
     * We don't set a XenStore watch on these paths in order to limit the
     * number of watches for performance reasons.
     */
    if (!remove && comp) {
        /*
         * TODO Replace this with a despatch table mapping XenStore keys to
         * callbacks.
         *
         * XXX physical_device_changed() and hotplug_status_changed() require
         * frontend() to have been called beforehand. This is achieved by
         * calling reconnect by calling reconnect() when the VBD is created.
         */
        if (!strcmp(PHYS_DEV_KEY, comp))
            err = physical_device_changed(device);
        else if (!strcmp(FRONTEND_KEY, comp))
            err = frontend(device);
		else if (!strcmp(HOTPLUG_STATUS_KEY, comp))
			err = hotplug_status_changed(device);
        else
            DBG(device, "ignoring '%s'\n", comp);
    }

    if (err && create && device) {
        int err2 = tapback_backend_destroy_device(device);
		if (err2)
			WARN(device, "error cleaning up: failed to destroy the device: "
					"%s\n", strerror(-err2));
	}
    return err;
}

/**
 * Scans XenStore and probes any device found.
 */
static int
tapback_domain_scan(backend_t *backend, const domid_t domid)
{
	char *path = NULL, **sub = NULL;
	int err = 0;
	unsigned i, n = 0;

	ASSERT(backend);

    if (tapback_is_master(backend)) {
        /*
         * FIXME The master tapback can reach this only if it has been
         * restarted. We need to figure out which slaves are running and
         * reconstruct state.
         */
        WARN(NULL, "master restart not yet implemented, ignoring domain %d\n",
                domid);
    } else {
        /*
         * Read the devices of this domain.
         */
        sub = xs_directory(backend->xs, XBT_NULL, backend->path, &n);
        err = -errno;
        free(path);

        if (!sub)
            goto out;

        /*
         * Probe each device.
         */
        for (i = 0; i < n; i++) {
            err = tapback_backend_probe_device(backend, domid, sub[i], NULL);
            if (unlikely(err))
                /*
                 * Keep probing other devices.
                 */
                WARN(NULL, "%s error probing device: %s\n",
                        sub[i], strerror(-err));
        }
    }

out:
	free(sub);
	return err;
}

/**
 * Compares the devices between XenStore and the device list, and
 * creates/destroys devices accordingly.
 */
static int
tapback_probe_domain(backend_t *backend, const domid_t domid)
{
    vbd_t *device = NULL, *next = NULL;
    int err;

    ASSERT(backend);

    /*
     * scrap all non-existent devices
     */
    tapback_backend_for_each_device(backend, device, next) {
        if (device->domid == domid) {
            err = tapback_backend_probe_device(backend, device->domid,
                    device->name, NULL);
            if (unlikely(err))
                /*
                 * Keep probing other devices.
                 */
                WARN(device, "error probing device: %s\n", strerror(-err));
        }
    }

    err = tapback_domain_scan(backend, domid);
    if (err == -ENOENT)
        err = 0;
    return err;
}

/**
 * Scans XenStore for all blktap3 devices and probes each one of them.
 *
 * @returns 0 on success, a negative error code otherwise
 *
 * XXX Only called by tapback_backend_handle_backend_watch.
 */
static int
tapback_backend_scan(backend_t *backend)
{
    vbd_t *device = NULL, *next = NULL;
    unsigned int i = 0, n = 0;
    char **dir = NULL;
    int err = 0;

	ASSERT(backend);

    DBG(NULL, "scanning entire back-end\n");

    if (!tapback_is_master(backend)) {
        /*
         * scrap all non-existent devices
         * TODO Why do we do this? Is this costly?
         */
        tapback_backend_for_each_device(backend, device, next) {
            /*
             * FIXME check that there is no compoment.
             */
            err = tapback_backend_probe_device(backend, device->domid,
                    device->name, NULL);
            if (unlikely(err))
                /*
                 * Keep probing other devices.
                 */
                WARN(device, "error probing device: %s\n", strerror(-err));
        }
    }

    /*
     * probe the new ones
     *
     * TODO We're checking each and every device in each and every domain,
     * could there be a performance issue in the presence of many VMs/VBDs?
     * (e.g. boot-storm)
     */
    if (!(dir = xs_directory(backend->xs, XBT_NULL,
                    backend->path, &n))) {
        err = -errno;
        if (err == -ENOENT)
            err = 0;
        else
            WARN(NULL, "error listing %s: %s\n", backend->path,
                    strerror(err));
        goto out;
    }

    DBG(NULL, "probing %d domains\n", n);

    for (i = 0; i < n; i++) { /* for each domain */
        char *end = NULL;
        domid_t domid = 0;

        /*
         * Get the domain ID.
         */
        domid = strtoul(dir[i], &end, 0);
        if (*end != 0 || end == dir[i])
            continue;

		err = tapback_domain_scan(backend, domid);
		if (err)
			WARN(NULL, "error scanning domain %d: %s\n", domid,
					strerror(-err));
    }

out:
    free(dir);
    return err;
}

int
tapback_backend_handle_backend_watch(backend_t *backend,
		char * const path)
{
    char *s = NULL, *end = NULL, *_path = NULL;
    domid_t domid = 0;
    int err = 0;
    bool exists = false;
    int n = 0;

	ASSERT(backend);
    ASSERT(path);

    _path = strdup(path);
    if (!_path) {
        err = -ENOMEM;
        goto out;
    }

    /*
     * path is something like "backend/vbd/domid/devid"
     */

    s = strtok(_path, "/");
	if (unlikely(!s)) {
		WARN(NULL, "invalid path %s\n", _path);
		err = -EINVAL;
		goto out;
	}
    ASSERT(!strcmp(s, XENSTORE_BACKEND));
    if (!(s = strtok(NULL, "/"))) {
        err = tapback_backend_scan(backend);
        goto out;
    }

    ASSERT(!strcmp(s, backend->name));
    if (!(s = strtok(NULL, "/"))) {
        err = tapback_backend_scan(backend);
        goto out;
    }

    domid = strtoul(s, &end, 0);
    if (*end != 0 || end == s) {
        WARN(NULL, "invalid domain ID \'%s\'\n", s);
        err = -EINVAL;
        goto out;
    }

    /*
     * The backend/vbd3/<domain ID> path was either created or removed.
     */
    n = s - _path + strlen(s);
    err = tapback_xs_exists(backend->xs, XBT_NULL, path, &n);
    if (err < 0) {
        WARN(NULL, "failed to read XenStore key %s: %s\n",
                &path[(s - _path)], strerror(-err));
        goto out;
    }
    if (err == 0)
        exists = false;
    else
        exists = true;
    err = 0;

    /*
     * Master tapback: check if there's tapback for this domain. If there isn't
     * one, create it, otherwise ignore this event, the per-domain tapback will
     * take care of it.
     */
    if (tapback_is_master(backend)) {
        struct backend_slave *slave = tapback_find_slave(backend, domid),
                             **_slave = NULL;

        if (!exists && slave) {
            DBG(NULL, "de-register slave[%d]\n", slave->master.pid);
            /*
             * remove the slave
             */
            tdelete(slave, &backend->master.slaves, compare);
            free(slave);
        }
        else if (exists && !slave) {
            pid_t pid;

            DBG(NULL, "need to create slave for domain %d\n", domid);

            pid = fork();
            if (pid == -1) {
                err = -errno;
                WARN(NULL, "failed to fork: %s\n", strerror(-err));
                goto out;
            } else if (pid != 0) { /* parent */
                slave = malloc(sizeof(*slave));
                if (!slave) {
                    int err2;
                    WARN(NULL, "failed to allocate memory\n");
                    err = -ENOMEM;
                    err2 = kill(pid, SIGKILL);
                    if (err2 != 0) {
                        err2 = errno;
                        WARN(NULL, "failed to kill process %d: %s "
                                "(error ignored)\n", pid, strerror(err2));
                    }
                    goto out;
                }
                slave->master.pid = pid;
                slave->master.domid = domid;
                _slave = tsearch(slave, &backend->master.slaves, compare);
                ASSERT(slave == *_slave);

                DBG(NULL, "created slave[%d] for domain %d\n",
                        slave->master.pid, slave->master.domid);

                /*
                 * FIXME Shall we watch the child process?
                 */
            } else { /* child */
                char *args[7];
                int i = 0;

                args[i++] = (char*)tapback_name;
                args[i++] = "-d";
                args[i++] = "-x";
                err = asprintf(&args[i++], "%d", domid);
                if (err == -1) {
                    err = -errno;
                    WARN(NULL, "failed to asprintf: %s\n", strerror(-err));
                    abort();
                }
                if (log_level == LOG_DEBUG)
                    args[i++] = "-v";
				if (!backend->barrier)
					args[i++] = "-b";
                args[i] = NULL;
                /*
                 * TODO we're hard-coding the name of the binary, better let
                 * the build system supply it.
                 */
                err = execvp(tapback_name, args);
                err = -errno;
                WARN(NULL, "failed to replace master process with slave: %s\n",
                        strerror(-err));
                abort();
            }
        }
        err = 0;
    } else {
        char *device = NULL;

        ASSERT(domid == backend->slave_domid);

        if (!exists) {
            /*
             * The entire domain may be removed in one go, so we need to tear
             * down all devices.
             */
            err = tapback_probe_domain(backend, domid);
            if (err)
                WARN(NULL, "failed to probe domain: %s\n", strerror(-err));

            /*
             * Time to go.
             */
            INFO(NULL, "domain removed, exit\n");
            tapback_backend_destroy(backend);
            exit(EXIT_SUCCESS);

            /*
             * R.I.P.
             */
        }

        /*
         * There's no device yet, the domain just got created, nothing to do
         * just yet. However, the entire sub-tree might have gotten created
         * before the slave so we still need to check whether there are any
         * devices.
         */
        device = strtok(NULL, "/");
        if (!device) {
            err = tapback_probe_domain(backend, domid);
            goto out;
        }

        /*
         * Create or remove a specific device.
         *
         * TODO tapback_backend_probe_device reads xenstore again to see if the
         * device should exist, but we already know that in the current
         * function.
         *
         * Optimise this case.
         */
        err = tapback_backend_probe_device(backend, domid, device,
                strtok(NULL, "/"));
    }
out:
    free(_path);
    return err;
}

bool
verbose(void)
{
    return log_level >= LOG_DEBUG;
}
