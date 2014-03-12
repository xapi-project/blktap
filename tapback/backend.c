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

#include "tapback.h"
#include "xenstore.h"
#include <xen/io/blkif.h>

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
                BLKTAP3_FRONTEND_TOKEN);

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
			if (err == -ESRCH) {
				/*
				 * TODO tapdisk might have died without cleaning up, in which
				 * case we'll receieve an I/O error when trying to talk to it
				 * through the socket, maybe search for a process with that
				 * PID? Alternatively, we can spawn tapdisks through a daemon
				 * which will monitor tapdisks for abrupt deaths and clean up
				 * after them (e.g. remove the socket).
				 */
				WARN(device, "tapdisk[%d] not running\n", device->tap->pid);
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
    struct list_head list;
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

    DBG(NULL, "%d/%s creating device\n", domid, name);

    if (!(device = calloc(1, sizeof(*device)))) {
        WARN(NULL, "error allocating memory\n");
        err = errno;
        goto out;
    }

	device->backend = backend;
    list_add_tail(&device->backend_entry, &device->backend->devices);

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
        WARN(NULL, "%d/%s: error creating device: %s\n", domid, name,
                strerror(-err));
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

/**
 * Retrieves the minor number and tapdisk-related information, and stores them
 * in @device.
 *
 * We might already have read the major/minor, but if the physical-device
 * key triggered we need to make sure it hasn't changed. This also protects us
 * against restarted transactions.
 *
 * TODO include domid/devid in messages
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int
physical_device(vbd_t *device) {

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

    DBG(device, "need to find tapdisk serving minor=%d\n", device->minor);

    device->tap = malloc(sizeof(*device->tap));
    if (!device->tap) {
        err = -ENOMEM;
        goto out;
    }
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

    if (device->sector_size & 0x1ff || device->sectors <= 0) {
        WARN(device, "warning: unexpected device characteristics: sector "
                "size=%d, sectors=%lu\n", device->sector_size,
				device->sectors);
    }

    /*
     * The front-end might have switched to state Connected before
     * physical-device is written. Check it's state and connect if necessary.
     *
     * FIXME This is not very efficient. Consider doing it like blkback: store
     * the state in the VBD in-memory structure.
     */
    if (device->frontend_state_path) {
        int err2 = -tapback_backend_handle_otherend_watch(device->backend,
                device->frontend_state_path);
        if (err2)
            WARN(device, "failed to switch state: %s\n", strerror(-err2));
    }
out:
    if (err) {
        device->major = device->minor = -1;
        free(device->tap);
        device->tap = NULL;
        device->sector_size = device->sectors = device->info = 0;
    }
    free(s);
    return err;
}

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
        /* TODO log error */
        err = -errno;
        goto out;
    }

    /*
     * We use the same token for all front-end watches. We don't have
     * to use a unique token for each front-end watch because when a
     * front-end watch fires we are given the XenStore path that
     * changed.
     */
    if (!xs_watch(device->backend->xs, device->frontend_state_path,
                BLKTAP3_FRONTEND_TOKEN)) {
        err = -errno;
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
 * Returns 0 in success, -errno on failure.
 */
static int
hotplug_status_changed(vbd_t * const device) {

	int err;
	char *hotplug_status = NULL;
	char *device_type = NULL;
	char *mode = NULL;

	ASSERT(device);

	hotplug_status = tapback_device_read(device, XBT_NULL, HOTPLUG_STATUS_KEY);
	if (!hotplug_status) {
		err = -errno;
		goto out;
	}
	if (!strcmp(hotplug_status, "connected")) {
		device->hotplug_status_connected = true;
		err = frontend_changed(device, device->frontend_state);
	}
	else {
		/*
		 * FIXME what other values can it receive?
		 */
		WARN(device, "invalid hotplug-status value %s\n", hotplug_status);
		err = -EINVAL;
	}

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

	err = tapback_device_printf(device, XBT_NULL, "info", false, "%d",
			device->info);
	if (err) {
		WARN(device, "failed to write info: %s\n", strerror(-err));
		goto out;
	}
out:
	free(hotplug_status);
	free(device_type);
	free(mode);

	return err;
}

/**
 * Attempts to reconnected the back-end to the fornt-end if possible (e.g.
 * after a tapback restart).
 *
 * returns 0 on success, an error code otherwise
 *
 * NB that 0 is also returned when a reconnection is not yet feasible
 */
static inline int
reconnect(vbd_t *device) {

    int err;

    err = physical_device(device);
    if (err) {
        if (err == -ENOENT)
            err = 0;
        else {
            WARN(device, "failed to retrieve physical device information: "
                    "%s\n", strerror(-err));
            goto out;
        }
    }
    err = frontend(device);
    if (err) {
        /*
         * The tapdisk or the front-end state path are not available.
         */
        if (err == -ENOENT || err == -ESRCH)
            err = 0;
        else
            WARN(device, "failed to watch front-end path: %s\n",
                    strerror(-err));
        goto out;
    }

    err = -tapback_backend_handle_otherend_watch(device->backend,
			device->frontend_state_path);
    if (err) {
        if (err == -ENOENT)
            err = 0;
        else
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

    DBG(NULL, "%d/%s probing device\n", domid, devname);

    /*
     * Ask XenStore if the device _should_ exist.
     */
    s = tapback_xs_read(backend->xs, XBT_NULL, "%s/%d/%s",
            backend->path, domid, devname);
    should_exist = s != NULL;
    free(s);

    /*
     * Search the device list for this specific device.
     *
     * TODO Use a faster data structure.
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
            WARN(NULL, "%d/%s error creating device: %s\n", domid, devname,
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
     * FIXME Why not set a watch on these paths when creating the device?  This
     * is how blkback does it. I suspect this has to do with avoiding setting
     * too many XenStore watches.
     */
    if (!remove && comp) {
        /*
         * TODO Replace this with a hash table mapping XenStore keys to
         * callbacks.
         */
        if (!strcmp(PHYS_DEV_KEY, comp))
            err = physical_device(device);
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
    unsigned int i = 0, j = 0, n = 0, m = 0;
    char **dir = NULL;
    int err = 0;

	ASSERT(backend);

    DBG(NULL, "scanning entire back-end\n");

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
        if (err) {
            WARN(device, "error probing device : %s\n", strerror(-err));
            /* TODO Should we fail in this case of keep probing? */
            goto out;
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
        char *path = NULL, **sub = NULL, *end = NULL;
        domid_t domid = 0;

        /*
         * Get the domain ID.
         */
        domid = strtoul(dir[i], &end, 0);
        if (*end != 0 || end == dir[i])
            continue;

        /*
         * Read the devices of this domain.
         */
        if (asprintf(&path, "%s/%d", backend->path, domid) == -1) {
            /* TODO log error */
            err = -errno;
            goto out;
        }
        sub = xs_directory(backend->xs, XBT_NULL, path, &m);
        err = -errno;
        free(path);

        if (!sub) {
            WARN(NULL, "error listing %s: %s\n", path, strerror(-err));
            goto out;
        }

        /*
         * Probe each device.
         */
        for (j = 0; j < m; j++) {
            /*
             * FIXME check that there is no compoment.
             */
            err = tapback_backend_probe_device(backend, domid, sub[j], NULL);
            if (err) {
                WARN(NULL, "%d/%s: error probing device %s of domain %d: %s\n",
                        domid, sub[j], strerror(-err));
                goto out;
            }
        }

        free(sub);
    }

out:
    free(dir);
    return err;
}

int
tapback_backend_handle_backend_watch(backend_t *backend,
		char * const path)
{
    char *s = NULL, *end = NULL, *name = NULL, *_path = NULL;
    domid_t domid = 0;
    int err = 0;

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
     * TODO Optimisation: since we know which domain changed, we don't have to
     * scan the whole thing. Add the domid as an optional parameter to
     * tapback_backend_scan.
     */
    if (!(name = strtok(NULL, "/"))) {
        err = tapback_backend_scan(backend);
        goto out;
    }

    /*
     * Create or remove a specific device.
     *
     * TODO tapback_backend_probe_device reads xenstore again to see if the
     * device should exist, but we already know that in the current function.
     * Optimise this case.
     */
    err = tapback_backend_probe_device(backend, domid, name,
			strtok(NULL, "/"));
out:
    free(_path);
    return err;
}
