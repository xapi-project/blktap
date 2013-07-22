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

/**
 * Removes the XenStore watch from the front-end.
 *
 * @param device the VBD whose front-end XenStore path should stop being
 * watched
 */
static void
tapback_device_unwatch_frontend_state(vbd_t * const device)
{
    assert(device);

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
    int err;

    assert(device);

    DBG("removing device %d/%s\n", device->domid, device->devid);

	list_del(&device->backend_entry);

    tapback_device_unwatch_frontend_state(device);

    /*
     * kill the tapdisk
     */
    err = tap_ctl_destroy(device->tap.pid, device->params, 0, NULL);
    if (err)
        WARN("failed to destroy tapdisk %d %s: %s (error ignored)\n",
                device->tap.pid, device->params, strerror(-err));

    free(device->frontend_path);
    free(device->name);
    free(device->type);
    free(device->path);
    free(device->params);
    free(device);
}

/**
 * Retrieves the tapdisk designated to serve this device, storing this
 * information in the supplied VBD handle.
 *
 * @param param <type>:/path/to/file
 * @param tap output parameter that receives the tapdisk process information.
 * The parameter is undefined when the function returns a non-zero value.
 * @returns 0 if a suitable tapdisk is found, ESRCH if no suitable tapdisk is
 * found, and an error code in case of error
 *
 * TODO rename function
 *
 * XXX Only called by blkback_probe.
 */
static inline int
blkback_find_tapdisk(const char *type, const char *path, tap_list_t *tap)
{
    struct list_head list;
    tap_list_t *_tap;
    int err;

    assert(type);
    assert(path);
    assert(tap);

    err = tap_ctl_list(&list);
    if (err) {
        WARN("error listing tapdisks: %s\n", strerror(err));
        goto out;
    }

    err = ESRCH;
    if (!list_empty(&list)) {
        tap_list_for_each_entry(_tap, &list) {
            if (_tap->type && !strcmp(_tap->type, type) && _tap->path
                    && !strcmp(_tap->path, path)) {
                err = 0;
                memcpy(tap, _tap, sizeof(tap));
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
 * @returns 0 on success, an error code otherwise
 */
static inline int
tapback_backend_create_device(const domid_t domid, const char * const name)
{
    vbd_t *device = NULL;
    int err = 0;

    assert(name);

    DBG("creating device domid=%d, devid=%s\n", domid, name);

    if (!(device = calloc(1, sizeof(*device)))) {
        WARN("error allocating memory\n");
        err = -errno;
        goto out;
    }

    device->domid = domid;

	list_add_tail(&device->backend_entry, &blktap3_daemon.devices);

    if (!(device->name = strdup(name))) {
        err = -errno;
        goto out;
    }

    /*
     * Get the front-end path from XenStore. We need this to talk to the
     * front-end.
     */
    if (!(device->frontend_path = tapback_device_read(device, "frontend"))) {
        err = errno;
        WARN("failed to read front-end path: %s\n", strerror(err));
        goto out;
    }

    /*
     * Get the file path backing the VBD.
     */
    device->params = tapback_xs_read(blktap3_daemon.xs, blktap3_daemon.xst,
            "%s/%d/%s/params", BLKTAP3_BACKEND_PATH, domid, name);
    if (!device->params) {
        err = errno;
        WARN("failed to read backing file: %s\n", strerror(err));
        goto out;
    }
    DBG("need to find tapdisk serving \'%s\'\n", device->params);

    err = parse_params(device->params, &device->type, &device->path);
    if (err) {
        WARN("failed to parse params \'%s\': %s\n", device->params,
                strerror(-err));
        goto out;
    }

    err = blkback_find_tapdisk(device->type, device->path, &device->tap);
    if (!err) {
        DBG("found tapdisk pid=%d\n", device->tap.pid);
    } else if (err == ESRCH) {
        /* TODO replace with tap_ctl_create */
        DBG("no such tapdisk\n");
        err = tap_ctl_spawn();
        if (err <= 0) {
            WARN("failed to create tapdisk: %s\n", strerror(-err));
            goto out;
        }
        device->tap.pid = err;
        DBG("spawned tapdisk pid=%d\n", device->tap.pid);
        err = tap_ctl_open(device->tap.pid, device->params, 0, NULL, NULL, 0);
        if (err) {
            WARN("failed to open %s on tapdisk pid=%d: %s\n", device->params,
                    device->tap.pid, strerror(-err));
            /* TODO The error handler assumes that there a device has been
             * opened, so tapdisk will complain that there is no such image.
             */
            goto out;
        }
        DBG("opened %s on tapdisk pid=%d\n", device->params, device->tap.pid);
    } else  {
        WARN("error looking for tapdisk: %s\n", strerror(err));
        goto out;
    }

    /*
     * get the VBD parameters from the tapdisk
     */
    if ((err = tap_ctl_info(device->tap.pid, device->params, &device->sectors,
                    &device->sector_size, &device->info))) {
        WARN("error retrieving disk characteristics: %s\n", strerror(-err));
        goto out;
    }

	/*
	 * Finally, watch the front-end path in XenStore for changes, i.e.
     * /local/domain/<domid>/device/vbd/<devname>/state
     * After this, we wait for the front-end to switch state to continue with
     * the initialisation.
	 */
    if (asprintf(&device->frontend_state_path, "%s/state",
                device->frontend_path) == -1) {
        /* TODO log error */
        err = -errno;
        goto out;
    }
    assert(device->frontend_state_path);

    /*
     * We use the same token for all front-end watches. We don't have to use a
     * unique token for each front-end watch because when a front-end watch
     * fires we are given the XenStore path that changed.
     */
    if (!xs_watch(blktap3_daemon.xs, device->frontend_state_path,
                BLKTAP3_FRONTEND_TOKEN)) {
        free(device->frontend_state_path);
        err = -errno;
        goto out;
    }

out:
    if (err) {
        WARN("error creating device: domid=%d name=%s err=%d (%s)\n",
                domid, name, err, strerror(err));
        if (device)
            tapback_backend_destroy_device(device);
    }
    return err;
}

/**
 * Creates (removes) a device depending on the existence (non-existence) of the
 * "backend/<backend name>/@domid/@devname" XenStore path.
 *
 * @param domid the ID of the domain where the VBD is created
 * @param devname device name
 * @returns 0 on success, an error code otherwise
 */
static int
tapback_backend_probe_device(const domid_t domid, const char * const devname)
{
    int should_exist = 0, create = 0, remove = 0;
    vbd_t *device = NULL;
    char * s = NULL;

    assert(devname);

    DBG("probing device domid=%d name=%s\n", domid, devname);

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

    if (!create && !remove) {
        /*
         * A watch has triggered on a path we're not interested in.
         * TODO Check if we can avoid probing the device completely based on
         * the path that triggered.
         */
        return 0;
    }

    /*
     * Remember that remove and create may both be true at the same time, as
     * this indicates that the device has been removed and re-created too fast.
     * In this case, we too need to remove and re-create the device,
     * respectively.
     */

    if (remove)
        tapback_backend_destroy_device(device);

    if (create) {
        const int err = tapback_backend_create_device(domid, devname);
        if (0 != err) {
            WARN("error creating device %s on domain %d: %s\n", devname, domid,
                    strerror(err));
            return err;
        }
    }

    return 0;
}

/**
 * Scans XenStore for all blktap3 devices and probes each one of them.
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
        err = tapback_backend_probe_device(device->domid, device->name);
        if (err) {
            WARN("error probing device %s of domain %d: %s\n", device->name,
                    device->domid, strerror(err));
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
        err = errno;
        if (err == ENOENT)
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
            err = errno;
            goto out;
        }
        sub = xs_directory(blktap3_daemon.xs, blktap3_daemon.xst, path, &m);
        err = errno;
        free(path);

        if (!sub) {
            WARN("error listing %s: %s\n", path, strerror(err));
            goto out;
        }

        /*
         * Probe each device.
         */
        for (j = 0; j < m; j++) {
            err = tapback_backend_probe_device(domid, sub[j]);
            if (err) {
                WARN("error probing device %s of domain %d: %s\n", sub[j],
                        domid, strerror(err));
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

    assert(path);

    s = strtok(path, "/");
    assert(!strcmp(s, XENSTORE_BACKEND));
    if (!(s = strtok(NULL, "/")))
        return tapback_backend_scan();

    assert(!strcmp(s, BLKTAP3_BACKEND_NAME));
    if (!(s = strtok(NULL, "/")))
        return tapback_backend_scan();

    domid = strtoul(s, &end, 0);
    if (*end != 0 || end == s) {
        WARN("invalid domain ID \'%s\'\n", s);
        return EINVAL;
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
    return tapback_backend_probe_device(domid, name);
}
