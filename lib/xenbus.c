/*
 * xenbus.c
 * 
 * xenbus interface to the blocktap.
 * 
 * this handles the top-half of integration with block devices through the
 * store -- the tap driver negotiates the device channel etc, while the
 * userland tap client needs to sort out the disk parameters etc.
 * 
 * (c) 2005 Andrew Warfield and Julian Chesterfield
 *
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
#include <stdlib.h>
#include <printf.h>
#include <string.h>
#include <err.h>
#include <stdarg.h>
#include <errno.h>
#include <xs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include "blktaplib.h"
#include "list.h"
#include "xs_api.h"

#if 1
#include <syslog.h>
#define DPRINTF(_f, _a...) syslog(LOG_INFO, _f, ##_a)
#else
#define DPRINTF(_f, _a...) ((void)0)
#endif

struct backend_info
{
	/* our communications channel */
	blkif_t *blkif;
	
	long int frontend_id;
	long int pdev;
	
	char *backpath;
	char *frontpath;
	
	struct list_head list;
};

static LIST_HEAD(belist);

static int strsep_len(const char *str, char c, unsigned int len)
{
	unsigned int i;
	
	for (i = 0; str[i]; i++)
		if (str[i] == c) {
			if (len == 0)
				return i;
			len--;
		}
	return (len == 0) ? i : -ERANGE;
}

static int get_be_id(const char *str)
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

static struct backend_info *be_lookup_be(const char *bepath)
{
	struct backend_info *be;
	
	list_for_each_entry(be, &belist, list)
		if (strcmp(bepath, be->backpath) == 0)
			return be;
	return (struct backend_info *)NULL;
}

static int be_exists_be(const char *bepath)
{
	return (be_lookup_be(bepath) != NULL);
}

static struct backend_info *be_lookup_fe(const char *fepath)
{
	struct backend_info *be;
	
	list_for_each_entry(be, &belist, list)
		if (strcmp(fepath, be->frontpath) == 0)
			return be;
	return (struct backend_info *)NULL;
}

static int backend_remove(struct backend_info *be)
{
	DPRINTF("%s: removing %s\n", __func__, be->backpath);

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

	return 0;
}

static void audit_backend_devices(struct xs_handle *h)
{
	unsigned int len;
	char *data, *path;
	struct backend_info *be, *tmp;

	list_for_each_entry_safe(be, tmp, &belist, list) {
		if (asprintf(&path, "%s/frontend-id", be->backpath) == -1)
			continue;

		data = xs_read(h, XBT_NULL, path, &len);
		free(path);

		if (data) {
			free(data);
			continue;
		}

		/* frontend-id removed, kill backend */
		if (errno == ENOENT) {
			DPRINTF("%s: removing backend: [%s]\n", 
				__func__, be->backpath);
			backend_remove(be);
		}
	}
}

static void handle_checkpoint_request(struct xs_handle *h, 
				      struct backend_info *binfo, char *node)
{
	int err;
	char *cp_uuid, *cpp, *rsp, *bepath = binfo->backpath;

	if (strcmp(node, "checkpoint/cp_uuid"))
		return;

	if (asprintf(&cpp, "%s/checkpoint", bepath) == -1)
		return;

	err = xs_gather(h, cpp, "rsp", NULL, &rsp, NULL);
	if (!err) {
		free(rsp);
		goto out;   /* request already serviced */
	}

	err = xs_gather(h, cpp, "cp_uuid", NULL, &cp_uuid, NULL);
	if (!err) {
		err = blkif_checkpoint(binfo->blkif, cp_uuid);
		xs_printf(h, cpp, "rsp", "%d", err);
		free(cp_uuid);
	}

 out:
	free(cpp);
}

static int handle_lock_request(struct xs_handle *h, char *bepath)
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
		DPRINTF("TAPDISK LOCK ERROR: expected lock for %s\n", bepath);
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

static void handle_shutdown_request(struct xs_handle *h,
				    struct backend_info *binfo, char *node)
{
	char *path;

	if (strcmp(node, "shutdown-tapdisk"))
		return;

	if (asprintf(&path, "%s/%s", binfo->backpath, node) == -1)
		return;

	if (!xs_exists(h, path))
		goto out;

	backend_remove(binfo);

 out:
	free(path);
}

/* Supply the information about the device to xenstore */
static int tapdisk_connect(struct xs_handle *h, struct backend_info *be)
{
	int err;
	char *path;

	if (!xs_printf(h, be->backpath, "sectors", "%llu",
		       be->blkif->ops->get_size(be->blkif))) {
		DPRINTF("ERROR: Failed writing sectors");
		return -1;
	}

	if (!xs_printf(h, be->backpath, "sector-size", "%lu",
		       be->blkif->ops->get_secsize(be->blkif))) {
		DPRINTF("ERROR: Failed writing sector-size");
		return -1;
	}

	if (!xs_printf(h, be->backpath, "info", "%u",
		       be->blkif->ops->get_info(be->blkif))) {
		DPRINTF("ERROR: Failed writing info");
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

static void ueblktap_setup(struct xs_handle *h, struct backend_info *be)
{
	char *path = NULL, *p, *dev, *bepath;
	int er, deverr;
	unsigned len;
	long int pdev = 0, handle;
	blkif_info_t *blk;

	bepath = be->backpath;

	deverr = xs_gather(h, bepath, "physical-device", "%li", &pdev, NULL);
	if (!deverr) {
		DPRINTF("pdev set to %ld\n",pdev);
		if (be->pdev && be->pdev != pdev) {
			DPRINTF("changing physical-device not supported");
			goto fail;
		}
		be->pdev = pdev;
	}

	if (be->blkif == NULL) {
		char mode;

		/* Front end dir is a number, which is used as the handle. */
		p = strrchr(be->frontpath, '/') + 1;
		handle = strtoul(p, NULL, 0);

		be->blkif = alloc_blkif(be->frontend_id);
		if (be->blkif == NULL)
			goto fail;

		be->blkif->be_id = get_be_id(bepath);
		
		/* Insert device specific info, */
		blk = calloc(1, sizeof(blkif_info_t));
		if (!blk) {
			DPRINTF("Out of memory - blkif_info_t\n");
			goto fail;
		}
		er = xs_gather(h, bepath, "params", NULL, &blk->params, NULL);
		if (er)
			goto fail;

		/* Check to see if device is to be opened read-only. */
		if (xs_gather(h, bepath, "mode", "%c", &mode, NULL))
			goto fail;
		if (mode == 'r')
			blk->readonly = 1;
		
		be->blkif->info = blk;
		
		if (deverr) {
			/*Dev number was not available, try to set manually*/
			pdev = convert_dev_name_to_num(blk->params);
			be->pdev = pdev;
		}

		be->blkif->backend_path = be->backpath;

		er = blkif_init(be->blkif, handle, be->pdev);
		if (er != 0) {
			DPRINTF("Unable to open device %s\n",blk->params);
			goto fail;
		}

		/* start locking if needed */
		er = handle_lock_request(h, bepath);
		if (er)
			goto fail;

		DPRINTF("[BECHG]: ADDED A NEW BLKIF (%s)\n", bepath);
	}

	if (!tapdisk_connect(h, be)) {
		DPRINTF("[SETUP] Complete\n\n");
		goto close;
	}

fail:
	if ( (be != NULL) && (be->blkif != NULL) ) 
		backend_remove(be);
close:
	if (path)
		free(path);
	return;
}

/* don't service requests if we've already shut down */
static int backend_ready(struct xs_handle *h, char *bepath)
{
	int ret = 0;
	char *path = NULL;

	if (asprintf(&path, "%s/shutdown-request", bepath) == -1) {
		DPRINTF("%s: asprintf failed: %d\n", __func__, errno);
		return 0;
	}

	if (xs_exists(h, path))
		goto out;

	free(path);
	if (asprintf(&path, "%s/shutdown-tapdisk", bepath) == -1) {
		DPRINTF("%s: asprintf failed: %d\n", __func__, errno);
		return 0;
	}

	if (xs_exists(h, path))
		goto out;

	free(path);
	if (asprintf(&path, "%s/shutdown-done", bepath) == -1) {
		DPRINTF("%s: asprintf failed: %d\n", __func__, errno);
		return 0;
	}

	if (xs_exists(h, path))
		goto out;

	ret = 1;

 out:
	free(path);
	return ret;
}

static int valid_start_request(struct xs_handle *h, char *bepath, char *node)
{
	if (strcmp(node, "frontend-id") && strcmp(node, "restart-tapdisk"))
		return 0;

	if (!backend_ready(h, bepath)) {
		DPRINTF("%s: got request %s, but backend %s "
			"not ready\n", __func__, node, bepath);
		return 0;
	}

	return 1;
}

static void handle_start_request(struct xs_handle *h, char *bepath)
{
	int err, fid;
	struct backend_info *be;
	char *frontpath, *backpath;

	DPRINTF("ADDING NEW DEVICE for %s\n", bepath);

	err = xs_gather(h, bepath, "frontend-id", "%li", &fid,
			"frontend", NULL, &frontpath, NULL);
	if (err) {
		DPRINTF("could not find frontend and frontend-id: %d\n", err);
		return;
	}

	backpath = strdup(bepath);
	if (!backpath) {
		DPRINTF("failed to allocate backpath: %d\n", errno);
		free(frontpath);
		return;
	}

	be = malloc(sizeof(struct backend_info));
	if (!be) {
		DPRINTF("failed to allocate new backend info: %d\n", errno);
		free(frontpath);
		free(backpath);
		return;
	}

	memset(be, 0, sizeof(struct backend_info));
	be->backpath    = backpath;
	be->frontpath   = frontpath;
	be->frontend_id = fid;

	list_add(&be->list, &belist);

	ueblktap_setup(h, be);
}

/**
 * Xenstore watch callback entry point. This code replaces the hotplug scripts,
 * and as soon as the xenstore backend driver entries are created, this script
 * gets called.
 */
static void ueblktap_probe(struct xs_handle *h, struct xenbus_watch *w, 
			   const char *bepath_im)
{
	int len;
	char *node, *bepath;
	struct backend_info *be;
	
	DPRINTF("ueblktap_probe %s\n", bepath_im);
	
	bepath = strdup(bepath_im);
	if (!bepath) {
		DPRINTF("No path\n");
		return;
	}

	/*
	 *asserts that xenstore structure is always 7 levels deep
	 *e.g. /local/domain/0/backend/vbd/1/2049
	 */
	len = strsep_len(bepath, '/', 7);
	if (len < 0) {
		audit_backend_devices(h);
		goto free;
	}
	bepath[len] = '\0';
	node = bepath + len + 1;

	/* Are we already tracking this device? */
	if ((be = be_lookup_be(bepath))) {
		DPRINTF("ueblktap_probe exists %s\n", bepath);

		/* check for snapshot request */
		handle_checkpoint_request(h, be, node);

		/* check for shutdown request */
		handle_shutdown_request(h, be, node);

	} else if (valid_start_request(h, bepath, node))
		handle_start_request(h, bepath);

 free:
	free(bepath);
}

/**
 *We set a general watch on the backend vbd directory
 *ueblktap_probe is called for every update
 *Our job is to monitor for new entries. As they 
 *are created, we initalise the state and attach a disk.
 */

int add_blockdevice_probe_watch(struct xs_handle *h, const char *domid)
{
	char *path;
	struct xenbus_watch *vbd_watch;
	
	if (asprintf(&path, "/local/domain/%s/backend/tap", domid) == -1)
		return -ENOMEM;
	
	vbd_watch = (struct xenbus_watch *)malloc(sizeof(struct xenbus_watch));
	if (!vbd_watch) {
		DPRINTF("ERROR: unable to malloc vbd_watch [%s]\n", path);
		return -EINVAL;
	}	
	vbd_watch->node     = path;
	vbd_watch->callback = ueblktap_probe;
	if (register_xenbus_watch(h, vbd_watch) != 0) {
		DPRINTF("ERROR: adding vbd probe watch %s\n", path);
		return -EINVAL;
	}
	return 0;
}

/* Asynch callback to check for /local/domain/<DOMID>/name */
void check_dom(struct xs_handle *h, struct xenbus_watch *w, 
	       const char *bepath_im)
{
	char *domid;

	domid = get_dom_domid(h);
	if (domid == NULL)
		return;

	add_blockdevice_probe_watch(h, domid);
	free(domid);
	unregister_xenbus_watch(h, w);
}

/* We must wait for xend to register /local/domain/<DOMID> */
int watch_for_domid(struct xs_handle *h)
{
	struct xenbus_watch *domid_watch;
	char *path = NULL;

	if (asprintf(&path, "/local/domain") == -1)
		return -ENOMEM;

	domid_watch = malloc(sizeof(struct xenbus_watch));
	if (domid_watch == NULL) {
		DPRINTF("ERROR: unable to malloc domid_watch [%s]\n", path);
		return -EINVAL;
	}	

	domid_watch->node     = path;
	domid_watch->callback = check_dom;

	if (register_xenbus_watch(h, domid_watch) != 0) {
		DPRINTF("ERROR: adding vbd probe watch %s\n", path);
		return -EINVAL;
	}

	DPRINTF("Set async watch for /local/domain\n");

	return 0;
}

int setup_probe_watch(struct xs_handle *h)
{
	char *domid;
	int ret;
	
	domid = get_dom_domid(h);
	if (domid == NULL)
		return watch_for_domid(h);

	ret = add_blockdevice_probe_watch(h, domid);
	free(domid);
	return ret;
}
