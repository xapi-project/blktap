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

#define PHYS_DEV_KEY "physical-device"

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
        xs_unwatch(blktap3_daemon.xs, device->frontend_state_path,
                BLKTAP3_FRONTEND_TOKEN);

    free(device->frontend_state_path);
    device->frontend_state_path = NULL;
}

/**
 * Destroys and deallocates the back-end part of a VBD.
 *
 * @param device the VBD to destroy
 */
static void
tapback_backend_destroy_device(vbd_t * const device)
{
    ASSERT(device);

    DBG("removing device %d/%d\n", device->domid, device->devid);

	list_del(&device->backend_entry);

    tapback_device_unwatch_frontend_state(device);

    free(device->frontend_path);
    free(device->name);
    free(device);
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
blkback_find_tapdisk(const int minor, tap_list_t *tap)
{
    struct list_head list;
    tap_list_t *_tap;
    int err;

    err = tap_ctl_list(&list);
    if (err) {
        WARN("error listing tapdisks: %s\n", strerror(-err));
        goto out;
    }

    err = -ESRCH;
    if (!list_empty(&list)) {
        tap_list_for_each_entry(_tap, &list) {
            if (_tap->minor == minor) {
                err = 0;
                memcpy(tap, _tap, sizeof(tap));
				DBG("found tapdisk[%d] minor=%d\n", _tap->pid, tap->minor);
                break;
            }
        }
        tap_ctl_list_free(&list);
    } else
        DBG("no tapdisks\n");

out:
    return err;
}

/**
 * Creates a device and adds it to the list of devices.
 * Initiates a XenStore watch to the front-end state.
 *
 * Creating the device implies initializing the handle and retrieving all the
 * information of the tapdisk serving this VBD.
 *
 * @param domid the ID of the domain where the VBD is created
 * @param name the name of the device
 * @returns 0 on success, a negative error code otherwise
 *
 * FIXME If the transaction fails with any error code other than EAGAIN, we're
 * not undoing everything we did in this function.
 */
static inline int
tapback_backend_create_device(const domid_t domid, const char * const name)
{
    vbd_t *device = NULL;
    int err = 0;
	char *s = NULL, *s2 = NULL;

    ASSERT(name);

    DBG("creating device %d/%s\n", domid, name);

    if (!(device = calloc(1, sizeof(*device)))) {
        WARN("error allocating memory\n");
        err = errno;
        goto out;
    }

	device->minor = -1;

	/* TODO check for errors */
	device->devid = atoi(name);

    device->domid = domid;

	list_add_tail(&device->backend_entry, &blktap3_daemon.devices);

    if (!(device->name = strdup(name))) {
        err = -errno;
        goto out;
    }

#if 0
	/*
	 * FIXME XAPI removes this path for forced shutdown on HVM guests.
	 */
	err = asprintf(&s2, "/local/domain/%d/device/vbd/%d", device->domid,
			device->devid);
	if (err == -1) {
		err = -errno;
		goto out;
	}
    if (!xs_watch(blktap3_daemon.xs, s2, FORCED_HVM_SHUTDOWN_TOKEN)) {
        err = -errno;
        goto out;
    }
#endif

out:
	free(s);
	free(s2);
    if (err) {
        WARN("error creating device %d/%s: %s\n", domid, name, strerror(-err));
        if (device)
            tapback_backend_destroy_device(device);
    }
    return err;
}

/**
 * Returns negative error code.
 */
static int
physical_device(vbd_t *device) {

    char * s = NULL, *p = NULL, *end = NULL;
	int err = 0;

	ASSERT(device);

	/*
	 * Get the minor.
	 */
	s = tapback_device_read(device, PHYS_DEV_KEY);
	if (!s) {
		err = -errno;
		WARN("failed to read the physical-device: %s\n",
				strerror(-err));
		goto out;
	}

	/*
	 * The XenStore key physical-device contains "major:minor" in hex.
	 */
	p = strchr(s, ':');
	if (!p) {
		WARN("malformed physical device '%s'\n", s);
		err = -EINVAL;
		goto out;
	}
	p++;
	device->minor = strtol(p, &end, 16);
	if (*end != 0 || end == p) {
		WARN("malformed physical device '%s'\n", s);
		err = -EINVAL;
		goto out;
	}

	DBG("need to find tapdisk serving minor %d\n", device->minor);

	err = blkback_find_tapdisk(device->minor, &device->tap);
	if (!err)
		DBG("found tapdisk[%d]\n", device->tap.pid);
	else  {
		WARN("error looking for tapdisk: %s\n", strerror(err));
		goto out;
	}

	/*
	 * get the VBD parameters from the tapdisk
	 */
	if ((err = tap_ctl_info(device->tap.pid, &device->sectors,
					&device->sector_size, &device->info,
					device->minor))) {
		WARN("error retrieving disk characteristics: %s\n",
				strerror(-err));
		goto out;
	}

	if (device->sector_size & 0x1ff || device->sectors <= 0) {
		WARN("warning: unexpected device characteristics: sector "
				"size=%d, sectors=%lu\n", device->sector_size,
				device->sectors);
	}
out:
	/*
	 * FIXME Clean up in case of error, e.g. zero out device member etc.
	 */
	free(s);
	return err;
}

static int
frontend(vbd_t *device) {

	int err = 0;

	ASSERT(device);

	/*
	 * FIXME Might already have executed this function and we're blindly
	 * overwriting device->frontend_path? Check the EEXIST error checked by
	 * xe_watch a bit further below in this function.
	 */

	/*
	 * Get the front-end path from XenStore. We need this to talk to
	 * the front-end.
	 */
	if (!(device->frontend_path = tapback_device_read(device,
					"frontend"))) {
		err = -errno;
		WARN("failed to read front-end path: %s\n", strerror(-err));
		goto out;
	}

	/*
	 * Finally, watch the front-end path in XenStore for changes, i.e.
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
	 * FIXME Handle gracefully.
	 */
	ASSERT(device->frontend_state_path);

	/*
	 * We use the same token for all front-end watches. We don't have
	 * to use a unique token for each front-end watch because when a
	 * front-end watch fires we are given the XenStore path that
	 * changed.
	 */
	if (!xs_watch(blktap3_daemon.xs, device->frontend_state_path,
				BLKTAP3_FRONTEND_TOKEN)) {
		if (errno == EEXIST)
			err = 0;
		else {
			err = -errno;
			goto out;
		}
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
tapback_backend_probe_device(const domid_t domid, const char * const devname,
		const char *comp)
{
    bool should_exist = false, create = false, remove = false;
	int err = 0;
    vbd_t *device = NULL;
    char * s = NULL;

    ASSERT(devname);

    DBG("probing device %d/%s\n", domid, devname);

    /*
     * Ask XenStore if the device _should_ exist.
     */
    s = tapback_xs_read(blktap3_daemon.xs, blktap3_daemon.xst, "%s/%d/%s",
            BLKTAP3_BACKEND_PATH, domid, devname);
    should_exist = s != NULL;
    free(s);

    /*
     * Search the device list for this specific device.
     */
    tapback_backend_find_device(device,
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
     * In this case, we too need to remove and re-create the device,
     * respectively.
     */

    if (remove)
        tapback_backend_destroy_device(device);

    if (create) {
        int err = tapback_backend_create_device(domid, devname);
        if (err) {
            WARN("error creating device %s on domain %d: %s\n", devname, domid,
                    strerror(-err));
            return err;
        }

		/*
		 * FIXME Should get this from tapback_backend_create_device.
		 */
		tapback_backend_find_device(device,
            device->domid == domid && !strcmp(device->name, devname));

		/*
		 * FIXME If the tapback daemon has just been restarted, we need to
		 * re-act upon all interesting XenStore keys.
		 */
		err = physical_device(device);
		if (err) {
			/*
			 * FIXME Clean up (e.g. free device). Include more information in
			 * the message.
			 */
			if (err == -ENOENT)
				err = 0;
			else {
				WARN("error initialising device: %s\n", strerror(-err));
				goto out;
			}
		}
		err = frontend(device);
		if (err) {
			/*
			 * FIXME Clean up (e.g. free device). Include more information in
			 * the message.
			 */
			if (err == -ENOENT)
				err = 0;
			else
				WARN("error initialising device: %s\n", strerror(-err));
			goto out;
		}
		err = -tapback_backend_handle_otherend_watch(
				device->frontend_state_path);
		if (err) {
			/*
			 * FIXME Clean up errors
			 */
			if (err == -ENOENT)
				err = 0;
			else
				WARN("error running the Xenbus protocol: %s\n",
						strerror(-err));
			goto out;
		}
	}

	/*
	 * Examine what happened to the XenStore component on which the watch
	 * triggered.
	 * TODO Why not set a watch on these paths when creating the device?
	 */
	if (!remove && comp) {
		/*
		 * Replace this with a hash table mapping XenStore keys to callbacks.
		 */
		if (!strcmp(PHYS_DEV_KEY, comp)) {
			err = physical_device(device);
		} else if (!strcmp("frontend", comp)) {
			err = frontend(device);
		} else {
			DBG("ignoring '%s'\n", comp);
		}
	}

out:
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
tapback_backend_scan(void)
{
    vbd_t *device = NULL, *next = NULL;
    unsigned int i = 0, j = 0, n = 0, m = 0;
    char **dir = NULL;
    int err = 0;

    DBG("scanning entire back-end\n");

    /*
     * scrap all non-existent devices
     * TODO Why do we do this? Is this costly?
     */

    tapback_backend_for_each_device(device, next) {
		/*
		 * FIXME check that there is no compoment.
		 */
        err = tapback_backend_probe_device(device->domid, device->name, NULL);
        if (err) {
            WARN("error probing device %s of domain %d: %s\n", device->name,
                    device->domid, strerror(-err));
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
    if (!(dir = xs_directory(blktap3_daemon.xs, blktap3_daemon.xst,
                    BLKTAP3_BACKEND_PATH, &n))) {
        err = -errno;
        if (err == -ENOENT)
            err = 0;
        else
            WARN("error listing %s: %s\n", BLKTAP3_BACKEND_PATH,
                    strerror(err));
        goto out;
    }

    DBG("probing %d domains\n", n);

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
        if (asprintf(&path, "%s/%d", BLKTAP3_BACKEND_PATH, domid) == -1) {
            /* TODO log error */
            err = -errno;
            goto out;
        }
        sub = xs_directory(blktap3_daemon.xs, blktap3_daemon.xst, path, &m);
        err = -errno;
        free(path);

        if (!sub) {
            WARN("error listing %s: %s\n", path, strerror(-err));
            goto out;
        }

        /*
         * Probe each device.
         */
        for (j = 0; j < m; j++) {
			/*
			 * FIXME check that there is no compoment.
			 */
            err = tapback_backend_probe_device(domid, sub[j], NULL);
            if (err) {
                WARN("error probing device %s of domain %d: %s\n", sub[j],
                        domid, strerror(-err));
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
tapback_backend_handle_backend_watch(char * const path)
{
    char *s = NULL, *end = NULL, *name = NULL;
    domid_t domid = 0;

    ASSERT(path);

	/*
	 * path is something like "backend/vbd/domid/devid"
	 */

    s = strtok(path, "/");
    ASSERT(!strcmp(s, XENSTORE_BACKEND));
    if (!(s = strtok(NULL, "/")))
        return tapback_backend_scan();

    ASSERT(!strcmp(s, BLKTAP3_BACKEND_NAME));
    if (!(s = strtok(NULL, "/")))
        return tapback_backend_scan();

    domid = strtoul(s, &end, 0);
    if (*end != 0 || end == s) {
        WARN("invalid domain ID \'%s\'\n", s);
        return -EINVAL;
    }

    /*
     * TODO Optimisation: since we know which domain changed, we don't have to
     * scan the whole thing. Add the domid as an optional parameter to
     * tapback_backend_scan.
     */
    if (!(name = strtok(NULL, "/")))
        return tapback_backend_scan();

    /*
     * Create or remove a specific device.
     *
     * TODO tapback_backend_probe_device reads xenstore again to see if the
     * device should exist, but we already know that in the current function.
     * Optimise this case.
     */
    return tapback_backend_probe_device(domid, name, strtok(NULL, "/"));
}
