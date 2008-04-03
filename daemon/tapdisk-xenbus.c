/*
 * (c) 2005 Andrew Warfield and Julian Chesterfield
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <xs.h>
#include <xen/io/xenbus.h>

#include "tapdisk-dispatch.h"

#include "list.h"
#include "xs_api.h"
#include "blktaplib.h"

#define CONTROL_WATCH_START         "tapdisk-watch-start"
#define CONTROL_WATCH_CHECKPOINT    "tapdisk-watch-checkpoint"
#define CONTROL_WATCH_PAUSE         "tapdisk-watch-pause"
#define CONTROL_WATCH_SHUTDOWN      "tapdisk-watch-shutdown"

struct backend_info {
	char              *backpath;
	char              *frontpath;

	blkif_t           *blkif;
	
	long int           frontend_id;
	long int           pdev;

	struct list_head   list;
};

static LIST_HEAD(belist);

static struct backend_info *
be_lookup_be(const char *bepath)
{
	struct backend_info *be;
	
	list_for_each_entry(be, &belist, list)
		if (strcmp(bepath, be->backpath) == 0)
			return be;

	return NULL;
}

static int
get_be_id(const char *str)
{
	int len,end;
	const char *ptr;
	char *tptr, num[10];
	
	len = strsep_len(str, '/', 6);
	end = strlen(str);
	if( (len < 0) || (end < 0) ) return -1;
	
	ptr = str + len + 1;
	strncpy(num, ptr, end - len);
	tptr = num + (end - (len + 1));
	*tptr = '\0';

	return atoi(num);
}

static void
tapdisk_xenbus_error(struct xs_handle *h, char *path, char *message)
{
	char *dir;

	if (asprintf(&dir, "%s/tapdisk-error", path) == -1) {
		EPRINTF("%s: failed to write %s\n", __func__, message);
		return;
	}

	xs_write(h, XBT_NULL, dir, message, strlen(message));

	free(dir);
}

static int
get_storage_type(struct xs_handle *h, const char *bepath)
{
	int type;
	unsigned int len;
	char *path, *stype;

	type = TAPDISK_STORAGE_TYPE_DEFAULT;

	if (asprintf(&path, "%s/sm-data/storage-type", bepath) == -1)
		return type;

	stype = xs_read(h, XBT_NULL, path, &len);
	if (!stype)
		goto out;
	else if (!strcmp(stype, "nfs"))
		type = TAPDISK_STORAGE_TYPE_NFS;
	else if (!strcmp(stype, "ext"))
		type = TAPDISK_STORAGE_TYPE_EXT;

out:
	free(path);
	free(stype);

	return type;
}

static void
tapdisk_remove_backend(char *path)
{
	struct backend_info *be;

	DPRINTF("%s: removing %s\n", __func__, path);

	be = be_lookup_be(path);
	if (!be)
		goto out;

	/* Unhook from be list. */
	list_del(&be->list);

	/* Free everything else. */
	if (be->blkif) {
		DPRINTF("Freeing blkif dev [%d]\n", be->blkif->minor - 1);
		free_blkif(be->blkif);
	}

	free(be->frontpath);
	free(be->backpath);
	free(be);

 out:
	tapdisk_control_stop();
}

static void
handle_checkpoint_event(struct xs_handle *h, char *path)
{
	int err;
	struct backend_info *be;
	char *cp_uuid, *cpp, *rsp;

	if (asprintf(&cpp, "%s/checkpoint", path) == -1)
		return;

	if (!xs_exists(h, cpp))
		goto out;

	be = be_lookup_be(path);
	if (!be) {
		EPRINTF("ERROR: got checkpoint request for non-existing "
			"backend %s\n", path);
		goto out;
	}

	err = xs_gather(h, cpp, "rsp", NULL, &rsp, NULL);
	if (!err) {
		free(rsp);
		goto out;   /* request already serviced */
	}

	err = xs_gather(h, cpp, "cp_uuid", NULL, &cp_uuid, NULL);
	if (!err) {
		err = blkif_checkpoint(be->blkif, cp_uuid);
		xs_printf(h, cpp, "rsp", "%d", err);
		free(cp_uuid);
	}

 out:
	free(cpp);
}

static int
handle_lock_event(struct xs_handle *h, char *bepath)
{
	int ret, enforce = 0;
	unsigned int len;
	char *lock, *lock_path;
	struct backend_info *binfo;

	binfo = be_lookup_be(bepath);
	if (!binfo)
		return -EINVAL;

	if (!xs_exists(h, "/local/tapdisk/VHD-expects-locking"))
		return 0;

	if (asprintf(&lock_path, "%s/lock", bepath) == -1)
		return -ENOMEM;

	lock = xs_read(h, XBT_NULL, lock_path, &len);
	if (!lock) {
		EPRINTF("TAPDISK LOCK ERROR: expected lock for %s\n", bepath);
		ret = (errno == ENOENT ? 0 : -errno);
		goto out;
	}

	if (xs_exists(h, "/local/tapdisk/VHD-enforce-locking"))
		enforce = 1;

	ret = blkif_lock(binfo->blkif, lock, enforce);

 out:
	free(lock);
	free(lock_path);
	return ret;
}

static void
handle_pause_event(struct xs_handle *h, char *id)
{
	int err;
	struct backend_info *be;
	char *pause = NULL, *pause_done = NULL;

	be = be_lookup_be(id);
	if (!be) {
		EPRINTF("got pause request for non-existing backend %s\n", id);
		err = -EINVAL;
		goto out;
	}

	if (asprintf(&pause, "%s/pause", id) == -1)
		pause = NULL;
	if (asprintf(&pause_done, "%s/pause-done", id) == -1)
		pause_done = NULL;
	if (!pause || !pause_done) {
		EPRINTF("allocating xenstore strings for %s pause\n", id);
		err = -ENOMEM;
		goto out;
	}

	if (xs_exists(h, pause)) {
		if (xs_exists(h, pause_done)) {
			EPRINTF("got pause request for paused vbd %s\n", id);
			err = -EINVAL;
			goto out;
		}

		DPRINTF("pausing %s\n", id);
		err = tapdisk_control_pause(be->blkif);
		if (err) {
			EPRINTF("failure pausing %s: %d\n", id, err);
			goto out;
		}

		if (!xs_write(h, XBT_NULL, pause_done, "", strlen(""))) {
			EPRINTF("writing pause_done for %s\n", id);
			err = -EIO;
			goto out;
		}

	} else if (xs_exists(h, pause_done)) {
		DPRINTF("resuming %s\n", id);
		err = tapdisk_control_resume(be->blkif);
		if (err) {
			EPRINTF("failure resuming %s: %d\n", id, err);
			goto out;
		}

		if (!xs_rm(h, XBT_NULL, pause_done)) {
			EPRINTF("removing pause_done for %s\n", id);
			err = -EIO;
			goto out;
		}
	}

	err = 0;

out:
	free(pause);
	free(pause_done);

	if (err)
		tapdisk_xenbus_error(h, id, "pause event failed");
}

static void
handle_shutdown_event(struct xs_handle *h, char *id)
{
	char *path;
	struct backend_info *be;

	if (asprintf(&path, "%s/shutdown-tapdisk", id) == -1) {
		EPRINTF("%s: out of memory\n", __func__);
		return;
	}

	if (!xs_exists(h, path))
		goto out;

	be = be_lookup_be(id);
	if (!be) {
		EPRINTF("ERROR: got shutdown request for non-existing "
			"backend: %s\n", id);
		goto out;
	}

	tapdisk_remove_backend(id);

 out:
	free(path);
}

/* Supply the information about the device to xenstore */
static int
tapdisk_connect(struct xs_handle *h, struct backend_info *be)
{
	int err;
	char *path;

	if (!xs_printf(h, be->backpath, "sectors", "%llu",
		       be->blkif->ops->get_size(be->blkif))) {
		EPRINTF("ERROR: Failed writing sectors");
		return -1;
	}

	if (!xs_printf(h, be->backpath, "sector-size", "%lu",
		       be->blkif->ops->get_secsize(be->blkif))) {
		EPRINTF("ERROR: Failed writing sector-size");
		return -1;
	}

	if (!xs_printf(h, be->backpath, "info", "%u",
		       be->blkif->ops->get_info(be->blkif))) {
		EPRINTF("ERROR: Failed writing info");
		return -1;
	}

	err = blkif_connected(be->blkif);
	if (err)
		goto clean;

	return 0;

 clean:
	if (asprintf(&path, "%s/info", be->backpath) == -1)
		return err;

	if (!xs_rm(h, XBT_NULL, path))
		goto clean_out;

	free(path);
	if (asprintf(&path, "%s/sector-size", be->backpath) == -1)
		return err;

	if (!xs_rm(h, XBT_NULL, path))
		goto clean_out;

	free(path);
	if (asprintf(&path, "%s/sectors", be->backpath) == -1)
		return err;

	xs_rm(h, XBT_NULL, path);

 clean_out:
	free(path);
	return err;
}

static int
tapdisk_add_blkif(struct xs_handle *h, struct backend_info *be)
{
	int err, deverr;
	struct blkif *blkif;
	long int handle, pdev;
	char mode, *p, *bepath;

	blkif  = NULL;
	bepath = be->backpath;
	deverr = xs_gather(h, bepath, "physical-device", "%li", &pdev, NULL);
	if (!deverr) {
		DPRINTF("pdev set to %ld\n", pdev);
		if (be->pdev && be->pdev != pdev) {
			EPRINTF("changing physical-device not supported");
			return -ENOSYS;
		}
		be->pdev = pdev;
	}

	if (be->blkif) {
		if (be->blkif->state != DISCONNECTED) {
			EPRINTF("Illegal restart request for %s\n", bepath);
			return -EINVAL;
		}
		goto connect;
	}

	/* Front end dir is a number, which is used as the handle. */
	p = strrchr(be->frontpath, '/') + 1;
	handle = strtoul(p, NULL, 0);

	blkif = alloc_blkif(be->frontend_id);
	if (blkif == NULL)
		goto fail;


	blkif->be_id        = get_be_id(bepath);
	blkif->backend_path = bepath;

	/* Insert device specific info */
	blkif->info = calloc(1, sizeof(struct blkif_info));
	if (!blkif->info) {
		EPRINTF("Out of memory - blkif_info_t\n");
		goto fail;
	}

	err = xs_gather(h, bepath, "params", NULL, &blkif->info->params, NULL);
	if (err)
		goto fail;

	/* Check to see if device is to be opened read-only. */
	if (xs_gather(h, bepath, "mode", "%c", &mode, NULL))
		goto fail;

	if (mode == 'r')
		blkif->info->readonly = 1;

	blkif->info->storage = get_storage_type(h, bepath);

	if (!be->pdev)
		/* Dev number was not available, try to set manually */
		be->pdev = convert_dev_name_to_num(blkif->info->params);

	be->blkif = blkif;
	err = blkif_init(blkif, handle, be->pdev);
	if (err != 0) {
		EPRINTF("Unable to open device %s\n", blkif->info->params);
		goto fail;
	}

	/* start locking if needed */
	err = handle_lock_event(h, bepath);
	if (err)
		goto fail;

	DPRINTF("ADDED A NEW BLKIF (%s)\n", bepath);

 connect:
	if (tapdisk_connect(h, be))
		goto fail;

	DPRINTF("[SETUP] Complete\n");

	return 0;

 fail:
	tapdisk_xenbus_error(h, bepath, "initializing tapdisk blkif");

	if (be->blkif)
		free_blkif(be->blkif);
	else if (blkif) {
		if (blkif->info) {
			free(blkif->info->params);
			free(blkif->info);
			blkif->info = NULL;
		}
		free(blkif);
	}

	be->blkif = NULL;
	return -EINVAL;
}

static struct backend_info *
tapdisk_add_backend(struct xs_handle *h, char *path)
{
	int err;
	struct backend_info *be;

	DPRINTF("ADDING NEW DEVICE for %s\n", path);

	be = calloc(1, sizeof(struct backend_info));
	if (!be) {
		EPRINTF("%s: out of memory\n", __func__);
		return NULL;
	}

	be->backpath = strdup(path);
	if (!be->backpath) {
		EPRINTF("%s: out of memory\n", __func__);
		goto fail;
	}

	err = xs_gather(h, path,
			"frontend", NULL, &be->frontpath, 
			"frontend-id", "%li", &be->frontend_id, NULL);
	if (err) {
		EPRINTF("could not find frontend and frontend-id: %d\n", err);
		goto fail;
	}

	list_add(&be->list, &belist);
	return be;

fail:
	tapdisk_xenbus_error(h, path, "initializing tapdisk backend");
	free(be->backpath);
	free(be->frontpath);
	free(be);
	return NULL;
}

/* don't service requests if we've already shut down */
static int
backend_ready(struct xs_handle *h, char *bepath)
{
	int state, ret = 0;
	char *path = NULL;

	if (asprintf(&path, "%s/shutdown-request", bepath) == -1) {
		EPRINTF("%s: asprintf failed: %d\n", __func__, errno);
		return 0;
	}

	if (xs_exists(h, path))
		goto out;

	free(path);
	if (asprintf(&path, "%s/shutdown-tapdisk", bepath) == -1) {
		EPRINTF("%s: asprintf failed: %d\n", __func__, errno);
		return 0;
	}

	if (xs_exists(h, path))
		goto out;

	free(path);
	if (asprintf(&path, "%s/shutdown-done", bepath) == -1) {
		EPRINTF("%s: asprintf failed: %d\n", __func__, errno);
		return 0;
	}

	if (xs_exists(h, path))
		goto out;

	ret = 1;

 out:
	free(path);
	return ret;
}

static int
valid_start_request(struct xs_handle *h, char *path)
{
	unsigned int len;
	char *spath, *start;

	if (asprintf(&spath, "%s/start-tapdisk", path) == -1) {
		EPRINTF("%s: out of memory\n", __func__);
		return 0;
	}

	start = xs_read(h, XBT_NULL, spath, &len);
	free(spath);

	if (!start)
		return 0;

	free(start);
	if (!backend_ready(h, path)) {
		EPRINTF("%s: got request %s, but backend "
			"not ready\n", __func__, path);
		return 0;
	}

	return 1;
}

static void
handle_start_event(struct xs_handle *h, char *id)
{
	int err;
	unsigned int len;

	if (valid_start_request(h, id)) {
		struct backend_info *be;

		be = be_lookup_be(id);
		if (!be)
			be = tapdisk_add_backend(h, id);

		if (be)
			if (tapdisk_add_blkif(h, be)) {
				EPRINTF("failed to add blkif for %s\n", id);
				tapdisk_remove_backend(id);
			}
	}
}

static int
tapdisk_check_uuid(struct xs_handle *h, const char *path, const char *uuid)
{
	int err;
	unsigned int len;
	char *cpath, *cuuid;

	cpath = NULL;
	cuuid = NULL;

	if (asprintf(&cpath, "%s/tapdisk-uuid", path) == -1) {
		EPRINTF("%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	cuuid = xs_read(h, XBT_NULL, cpath, &len);
	if (!cuuid) {
		EPRINTF("could not find tapdisk-uuid\n");
		err = -ENOENT;
		goto out;
	}

	if (strcmp(uuid, cuuid)) {
		EPRINTF("uuid mismatch: %s, %s\n", uuid, cuuid);
		err = -EINVAL;
		goto out;
	}

	err = 0;

 out:
	free(cpath);
	free(cuuid);
	return err;
}

int
tapdisk_control_handle_event(struct xs_handle *h, const char *uuid)
{
	int ret, len;
	unsigned int num;
	char **res, *path, *token, *id;

	res = xs_read_watch(h, &num);
	if (!res)
		return -EAGAIN;

	ret   = 0;
	id    = NULL;
	path  = res[XS_WATCH_PATH];
	token = res[XS_WATCH_TOKEN];

	DPRINTF("%s: watch: %s, path: %s\n", __func__, token, path);

	len = strsep_len(path, '/', 7);
	if (len < 0)
		goto out;

	id = strdup(path);
	if (!id)
		goto out;
	id[len] = '\0';

	if (!xs_exists(h, id)) {
		tapdisk_remove_backend(id);
		goto out;
	}

	if (tapdisk_check_uuid(h, id, uuid)) {
		tapdisk_remove_backend(id);
		goto out;
	}

	if (!strcmp(token, CONTROL_WATCH_START))
		handle_start_event(h, id);
	else if (!strcmp(token, CONTROL_WATCH_CHECKPOINT))
		handle_checkpoint_event(h, id);
	else if (!strcmp(token, CONTROL_WATCH_PAUSE))
		handle_pause_event(h, id);
	else if (!strcmp(token, CONTROL_WATCH_SHUTDOWN))
		handle_shutdown_event(h, id);

out:
	free(id);
	free(res);
	return 0;
}

int
add_control_watch(struct xs_handle *h, const char *path, const char *uuid)
{
	char *watch;

	if (tapdisk_check_uuid(h, path, uuid))
		return -EINVAL;

	/* start event */
	if (asprintf(&watch, "%s/start-tapdisk", path) == -1)
		goto mem_fail;

	if (!xs_watch(h, watch, CONTROL_WATCH_START))
		goto watch_fail;

	free(watch);

	/* checkpoint event */
	if (asprintf(&watch, "%s/cp_uuid", path) == -1)
		goto mem_fail;

	if (!xs_watch(h, watch, CONTROL_WATCH_CHECKPOINT))
		goto watch_fail;

	free(watch);

	/* tapdisk pause event */
	if (asprintf(&watch, "%s/pause", path) == -1)
		goto mem_fail;

	if (!xs_watch(h, watch, CONTROL_WATCH_PAUSE))
		goto watch_fail;

	free(watch);

	/* shutdown event */
	if (asprintf(&watch, "%s/shutdown-tapdisk", path) == -1)
		goto mem_fail;

	if (!xs_watch(h, watch, CONTROL_WATCH_SHUTDOWN))
		goto watch_fail;

	free(watch);

	return 0;

mem_fail:
	EPRINTF("%s: failed to allocate watch for %s\n", __func__, path);
	return -ENOMEM;

watch_fail:
	EPRINTF("%s: failed to set watch for %s\n", __func__, watch);
	free(watch);
	return -EINVAL;
}

int
remove_control_watch(struct xs_handle *h, const char *path)
{
	char *watch;

	/* start event */
	if (asprintf(&watch, "%s/start-tapdisk", path) == -1)
		goto mem_fail;

	if (!xs_unwatch(h, watch, CONTROL_WATCH_START))
		goto watch_fail;

	free(watch);

	/* checkpoint event */
	if (asprintf(&watch, "%s/cp_uuid", path) == -1)
		goto mem_fail;

	if (!xs_unwatch(h, watch, CONTROL_WATCH_CHECKPOINT))
		goto watch_fail;

	free(watch);

	/* tapdisk pause event */
	if (asprintf(&watch, "%s/pause", path) == -1)
		goto mem_fail;

	if (!xs_unwatch(h, watch, CONTROL_WATCH_PAUSE))
		goto watch_fail;

	free(watch);

	/* shutdown event */
	if (asprintf(&watch, "%s/shutdown-tapdisk", path) == -1)
		goto mem_fail;

	if (!xs_unwatch(h, watch, CONTROL_WATCH_SHUTDOWN))
		goto watch_fail;

	free(watch);

	return 0;

mem_fail:
	EPRINTF("%s: failed to remove watch for %s\n", __func__, path);
	return -ENOMEM;

watch_fail:
	EPRINTF("%s: failed to remove watch for %s\n", __func__, watch);
	free(watch);
	return -EINVAL;	
}
