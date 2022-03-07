/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <unistd.h>
#include <stdlib.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "debug.h"
#include "libvhd.h"
#include "tapdisk-blktap.h"
#include "tapdisk-image.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-vbd.h"
#include "tapdisk-metrics.h"
#include "tapdisk-disktype.h"
#include "tapdisk-interface.h"
#include "tapdisk-stats.h"
#include "tapdisk-storage.h"
#include "tapdisk-nbdserver.h"
#include "td-stats.h"
#include "tapdisk-utils.h"

#define DBG(_level, _f, _a...) tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...) tlog_error(_err, _f, ##_a)

#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "vbd: " _f, ##_a)
#define ERROR(_f, _a...)           tlog_syslog(TLOG_WARN, "vbd: " _f, ##_a)

#define TD_VBD_EIO_RETRIES          10
#define TD_VBD_EIO_SLEEP            1
#define TD_VBD_WATCHDOG_TIMEOUT     10

char* op_strings[TD_OPS_END] ={"read", "write", "block_status"};

static void tapdisk_vbd_complete_vbd_request(td_vbd_t *, td_vbd_request_t *);
static int  tapdisk_vbd_queue_ready(td_vbd_t *);
static void tapdisk_vbd_check_queue_state(td_vbd_t *);

static bool log=true;

/*
 * initialization
 */

static void
tapdisk_vbd_mark_progress(td_vbd_t *vbd)
{
	gettimeofday(&vbd->ts, NULL);
}

td_vbd_t*
tapdisk_vbd_create(uint16_t uuid)
{
	td_vbd_t *vbd;

	vbd = calloc(1, sizeof(td_vbd_t));
	if (!vbd) {
		EPRINTF("failed to allocate tapdisk state\n");
		return NULL;
	}

	shm_init(&vbd->rrd.shm);

	vbd->uuid        = uuid;
	vbd->req_timeout = TD_VBD_REQUEST_TIMEOUT;
	vbd->watchdog_warned = false;

	INIT_LIST_HEAD(&vbd->images);
	INIT_LIST_HEAD(&vbd->new_requests);
	INIT_LIST_HEAD(&vbd->pending_requests);
	INIT_LIST_HEAD(&vbd->failed_requests);
	INIT_LIST_HEAD(&vbd->completed_requests);
	INIT_LIST_HEAD(&vbd->next);
	INIT_LIST_HEAD(&vbd->rings);
	INIT_LIST_HEAD(&vbd->dead_rings);
	tapdisk_vbd_mark_progress(vbd);

	return vbd;
}

int
tapdisk_vbd_initialize(int rfd, int wfd, uint16_t uuid)
{
	td_vbd_t *vbd;

	vbd = tapdisk_server_get_vbd(uuid);
	if (vbd) {
		EPRINTF("duplicate vbds! %u\n", uuid);
		return -EEXIST;
	}

	vbd = tapdisk_vbd_create(uuid);
	if (!vbd) {
		EPRINTF("failed to create vbd\n");
		return -ENOMEM;
	}

	tapdisk_server_add_vbd(vbd);

	return 0;
}

static inline void
tapdisk_vbd_add_image(td_vbd_t *vbd, td_image_t *image)
{
	list_add_tail(&image->next, &vbd->images);
}

static inline int
tapdisk_vbd_is_last_image(td_vbd_t *vbd, td_image_t *image)
{
	return list_is_last(&image->next, &vbd->images);
}

static inline td_image_t *
tapdisk_vbd_first_image(td_vbd_t *vbd)
{
	td_image_t *image = NULL;
	if (!list_empty(&vbd->images))
		image = list_entry(vbd->images.next, td_image_t, next);
	return image;
}

static inline td_image_t *
tapdisk_vbd_last_image(td_vbd_t *vbd)
{
	td_image_t *image = NULL;
	if (!list_empty(&vbd->images))
		image = list_entry(vbd->images.prev, td_image_t, next);
	return image;
}

static inline td_image_t *
tapdisk_vbd_next_image(td_image_t *image)
{
	return list_entry(image->next.next, td_image_t, next);
}

static int
tapdisk_vbd_validate_chain(td_vbd_t *vbd)
{
	return tapdisk_image_validate_chain(&vbd->images);
}

static int
vbd_stats_destroy(td_vbd_t *vbd) {

    int err = 0;

    ASSERT(vbd);

    err = shm_destroy(&vbd->rrd.shm);
    if (unlikely(err)) {
        EPRINTF("failed to destroy RRD file: %s\n", strerror(err));
        goto out;
    }

    free(vbd->rrd.shm.path);
    vbd->rrd.shm.path = NULL;

out:
    return -err;
}

static int
vbd_stats_create(td_vbd_t *vbd) {

    int err;

    ASSERT(vbd);

	err = mkdir("/dev/shm/metrics", S_IRUSR | S_IWUSR);
	if (likely(err)) {
        err = errno;
        if (unlikely(err != EEXIST))
    		goto out;
        else
            err = 0;
    }

    /*
     * FIXME Rename this to something like "vbd3-domid-devid". Consider
     * consolidating this with the io_ring shared memory file. Check if blkback
     * exports the same information in some sysfs file and if so move this to
     * the ring location.
     */
    err = asprintf(&vbd->rrd.shm.path, "/dev/shm/metrics/tap-%d-%d", getpid(),
            vbd->uuid);
    if (err == -1) {
        err = errno;
        vbd->rrd.shm.path = NULL;
        EPRINTF("failed to create metric file: %s\n", strerror(err));
        goto out;
    }
    err = 0;

    vbd->rrd.shm.size = PAGE_SIZE;
    err = shm_create(&vbd->rrd.shm);
    if (err)
        EPRINTF("failed to create RRD: %s\n", strerror(err));

out:
    if (err) {
        int err2 = vbd_stats_destroy(vbd);
        if (err2)
            EPRINTF("failed to clean up failed RRD shared memory creation: "
                    "%s (error ignored)\n", strerror(-err2));
    }
    return -err;
}

void
tapdisk_vbd_close_vdi(td_vbd_t *vbd)
{
    int err;

    err = vbd_stats_destroy(vbd);
    if (err) {
        EPRINTF("failed to destroy RRD stats file: %s (error ignored)\n",
                strerror(-err));
    }

    err = td_metrics_vdi_stop(&vbd->vdi_stats);
    if (err) {
        EPRINTF("failed to destroy stats file: %s\n", strerror(-err));
    }

	tapdisk_image_close_chain(&vbd->images);

	if (vbd->secondary &&
	    vbd->secondary_mode != TD_VBD_SECONDARY_MIRROR) {
		tapdisk_image_close(vbd->secondary);
		vbd->secondary = NULL;
	}

	if (vbd->retired) {
		tapdisk_image_close(vbd->retired);
		vbd->retired = NULL;
	}

	td_flag_set(vbd->state, TD_VBD_CLOSED);
}

static int
tapdisk_vbd_add_block_cache(td_vbd_t *vbd)
{
	td_image_t *cache, *image, *target, *tmp;
	int err;

	target = NULL;

	tapdisk_vbd_for_each_image(vbd, image, tmp)
		if (td_flag_test(image->flags, TD_OPEN_RDONLY) &&
		    td_flag_test(image->flags, TD_OPEN_SHAREABLE)) {
			target = image;
			break;
		}

	if (!target)
		return 0;

	cache = tapdisk_image_allocate(target->name,
				       DISK_TYPE_BLOCK_CACHE,
				       target->flags);
	if (!cache)
		return -ENOMEM;

	/* try to load existing cache */
	err = td_load(cache);
	if (!err)
		goto done;

	/* hack driver to send open() correct image size */
	if (!target->driver) {
		err = -ENODEV;
		goto fail;
	}

	cache->driver = tapdisk_driver_allocate(cache->type,
						cache->name,
						cache->flags);
	if (!cache->driver) {
		err = -ENOMEM;
		goto fail;
	}

	cache->driver->info = target->driver->info;

	/* try to open new cache */
	err = td_open(cache, &vbd->encryption);
	if (!err)
		goto done;

fail:
	/* give up */
	tapdisk_image_free(target);
	tapdisk_image_free(cache);
	return err;

done:
	/* insert cache before image */
	list_add(&cache->next, target->next.prev);
	return 0;
}

static int
tapdisk_vbd_add_local_cache(td_vbd_t *vbd)
{
	td_image_t *cache, *parent;
	int err;

	parent = tapdisk_vbd_first_image(vbd);
	if (tapdisk_vbd_is_last_image(vbd, parent)) {
		DPRINTF("Single-image chain, nothing to cache");
		return 0;
	}

	cache = tapdisk_image_allocate(parent->name,
				       DISK_TYPE_LCACHE,
				       parent->flags);

	if (!cache)
		return -ENOMEM;

	/* try to load existing cache */
	err = td_load(cache);
	if (!err)
		goto done;

	cache->driver = tapdisk_driver_allocate(cache->type,
						cache->name,
						cache->flags);
	if (!cache->driver) {
		err = -ENOMEM;
		goto fail;
	}

	cache->driver->info = parent->driver->info;

	/* try to open new cache */
	err = td_open(cache, &vbd->encryption);
	if (!err)
		goto done;

fail:
	tapdisk_image_free(cache);
	return err;

done:
	/* insert cache right above leaf image */
	list_add(&cache->next, &parent->next);

	DPRINTF("Added local_cache driver\n");
	return 0;
}

int
tapdisk_vbd_add_secondary(td_vbd_t *vbd)
{
	td_image_t *leaf, *second = NULL;
	const char *path;
	int type, err;

	if (strcmp(vbd->secondary_name, "null") == 0) {
		DPRINTF("Removing secondary image\n");
		vbd->secondary_mode = TD_VBD_SECONDARY_DISABLED;
		vbd->secondary = NULL;
		vbd->nbd_mirror_failed = 0;
		return 0;
	}

	DPRINTF("Adding secondary image: %s\n", vbd->secondary_name);

	type = tapdisk_disktype_parse_params(vbd->secondary_name, &path);
	if (type < 0)
		return type;

	leaf = tapdisk_vbd_first_image(vbd);
	if (!leaf) {
		err = -EINVAL;
		goto fail;
	}

	err = tapdisk_image_open(type, path, leaf->flags, &vbd->encryption, &second);
	if (err) {
		if (type == DISK_TYPE_NBD)
			vbd->nbd_mirror_failed = 1;

		vbd->secondary=NULL;
		vbd->secondary_mode=TD_VBD_SECONDARY_DISABLED;
		
		goto fail;
	}

	if (second->info.size != leaf->info.size) {
		EPRINTF("Secondary image size %"PRIu64" != image size %"PRIu64"\n",
			second->info.size, leaf->info.size);
		err = -EINVAL;
		goto fail;
	}

	vbd->secondary = second;
	leaf->flags |= TD_IGNORE_ENOSPC;
	if (td_flag_test(vbd->flags, TD_OPEN_STANDBY)) {
		DPRINTF("In standby mode\n");
		vbd->secondary_mode = TD_VBD_SECONDARY_STANDBY;
	} else {
		DPRINTF("In mirror mode\n");
		vbd->secondary_mode = TD_VBD_SECONDARY_MIRROR;
		/*
		 * we actually need this image to also be part of the chain, 
		 * since it may already contain data
		 */
		list_add(&second->next, &leaf->next);
	}

	DPRINTF("Added secondary image\n");
	return 0;

fail:
	if (second)
		tapdisk_image_close(second);
	return err;
}

static void signal_enospc(td_vbd_t *vbd)
{
	int fd, err;
	char *fn;

	err = asprintf(&fn, BLKTAP2_ENOSPC_SIGNAL_FILE"%d", vbd->tap->minor);
	if (err == -1) {
		EPRINTF("Failed to signal ENOSPC condition\n");
		return;
	}

	fd = open(fn, O_WRONLY | O_CREAT | O_NONBLOCK, 0666);
	if (fd == -1)
		EPRINTF("Failed to open file to signal ENOSPC condition\n");
	else
		close(fd);

	free(fn);
}

#if 0
static int
tapdisk_vbd_open_index(td_vbd_t *vbd)
{
	int err;
	char *path;
	td_flag_t flags;
	td_image_t *last, *image;

	last = tapdisk_vbd_last_image(vbd);
	err  = asprintf(&path, "%s.bat", last->name);
	if (err == -1)
		return -errno;

	err = access(path, R_OK);
	if (err == -1) {
		free(path);
		return -errno;
	}

	flags = vbd->flags | TD_OPEN_RDONLY | TD_OPEN_SHAREABLE;
	image = tapdisk_image_allocate(path, DISK_TYPE_VINDEX, flags);
	if (!image) {
		err = -ENOMEM;
		goto fail;
	}

	err = td_open(image);
	if (err)
		goto fail;

	tapdisk_vbd_add_image(vbd, image);
	return 0;

fail:
	if (image)
		tapdisk_image_free(image);
	free(path);
	return err;
}
#endif

static int tapdisk_vbd_add_dirty_log(td_vbd_t *vbd)
{
	int err;
	td_driver_t *driver;
	td_image_t *log, *parent;

	driver = NULL;
	log    = NULL;

	DPRINTF("CBT:tapdisk_vbd_add_dirty_log called for %s with log file %s\n",
			vbd->name, vbd->logpath);

	parent = tapdisk_vbd_first_image(vbd);

	DPRINTF("CBT: Size in Image: %"PRIu64" sectors\n", parent->info.size);

	log = tapdisk_image_allocate(vbd->logpath,
					DISK_TYPE_LOG,
					parent->flags);
	if (!log)
		return -ENOMEM;

	driver = tapdisk_driver_allocate(log->type,
					 log->name,
					 log->flags);
	if (!driver) {
		err = -ENOMEM;
		goto fail;
	}

	driver->info = parent->driver->info;
	log->driver  = driver;

	err = td_open(log, &vbd->encryption);
	if (err)
		goto fail;

	/* insert log before image */
	list_add(&log->next, parent->next.prev);
	return 0;

fail:
	tapdisk_image_free(log);
	return err;
}

int 
tapdisk_vbd_open_vdi(td_vbd_t *vbd, const char *name, td_flag_t flags, int prt_devnum)
{
	char *tmp = vbd->name;
	int err;

	if (!list_empty(&vbd->images)) {
		err = -EBUSY;
		goto fail;
	}

	if (!name && !vbd->name) {
		err = -EINVAL;
		goto fail;
	}

	if (name) {
		vbd->name = strdup(name);
		if (!vbd->name) {
			err = -errno;
			goto fail;
		}
	}

	err = tapdisk_image_open_chain(vbd->name, flags, prt_devnum, &vbd->encryption, &vbd->images);
	if (err)
		goto fail;

	td_flag_clear(vbd->state, TD_VBD_CLOSED);
	vbd->flags = flags;

	if (td_flag_test(vbd->flags, TD_OPEN_ADD_LOG)) {
		if (!vbd->logpath) {
			err = -EINVAL;
			goto fail;
		}
		err = tapdisk_vbd_add_dirty_log(vbd);
		if (err)
			goto fail;
	}

	if (td_flag_test(vbd->flags, TD_OPEN_ADD_CACHE)) {
		err = tapdisk_vbd_add_block_cache(vbd);
		if (err)
			goto fail;
	}

	if (td_flag_test(vbd->flags, TD_OPEN_LOCAL_CACHE)) {
		err = tapdisk_vbd_add_local_cache(vbd);
		if (err)
			goto fail;
	}

	err = tapdisk_vbd_validate_chain(vbd);
	if (err)
		goto fail;

	if (td_flag_test(vbd->flags, TD_OPEN_SECONDARY)) {
		err = tapdisk_vbd_add_secondary(vbd);
		if (err) {
			if (vbd->nbd_mirror_failed != 1)
				goto fail;
			INFO("Ignoring failed NBD secondary attach\n");
			err = 0;
		}
	}

	err = vbd_stats_create(vbd);
	if (err)
		goto fail;

	err = td_metrics_vdi_start(vbd->tap->minor, &vbd->vdi_stats);
	if (err)
		goto fail;
	if (tmp != vbd->name)
		free(tmp);

	return err;

fail:
	if (vbd->name != tmp) {
		free(vbd->name);
		vbd->name = tmp;
	}

	if (!list_empty(&vbd->images))
		tapdisk_image_close_chain(&vbd->images);

	vbd->flags = 0;

	return err;
}

void
tapdisk_vbd_detach(td_vbd_t *vbd)
{
	td_blktap_t *tap = vbd->tap;

	if (tap) {
		tapdisk_blktap_close(tap);
		vbd->tap = NULL;
	}
}

int
tapdisk_vbd_attach(td_vbd_t *vbd, const char *devname, int minor)
{

	if (vbd->tap)
		return -EALREADY;

	return tapdisk_blktap_open(devname, vbd, &vbd->tap);
}

/*
int
tapdisk_vbd_open(td_vbd_t *vbd, const char *name,
		 int minor, const char *ring, td_flag_t flags)
{
	int err;

	err = tapdisk_vbd_open_vdi(vbd, name, flags, -1);
	if (err)
		goto out;

	err = tapdisk_vbd_attach(vbd, ring, minor);
	if (err)
		goto out;

	return 0;

out:
	tapdisk_vbd_detach(vbd);
	tapdisk_vbd_close_vdi(vbd);
	free(vbd->name);
	vbd->name = NULL;
	return err;
}
*/

static void
tapdisk_vbd_queue_count(td_vbd_t *vbd, int *new,
			int *pending, int *failed, int *completed)
{
	int n, p, f, c;
	td_vbd_request_t *vreq, *tvreq;

	n = 0;
	p = 0;
	f = 0;
	c = 0;

	tapdisk_vbd_for_each_request(vreq, tvreq, &vbd->new_requests)
		n++;

	tapdisk_vbd_for_each_request(vreq, tvreq, &vbd->pending_requests)
		p++;

	tapdisk_vbd_for_each_request(vreq, tvreq, &vbd->failed_requests)
		f++;

	tapdisk_vbd_for_each_request(vreq, tvreq, &vbd->completed_requests)
		c++;

	*new       = n;
	*pending   = p;
	*failed    = f;
	*completed = c;
}

static int
tapdisk_vbd_shutdown(td_vbd_t *vbd)
{
	int new, pending, failed, completed;

	if (!list_empty(&vbd->pending_requests))
		return -EAGAIN;

	tapdisk_vbd_queue_count(vbd, &new, &pending, &failed, &completed);

	DPRINTF("%s: state: 0x%08x, new: 0x%02x, pending: 0x%02x, "
		"failed: 0x%02x, completed: 0x%02x\n", 
		vbd->name, vbd->state, new, pending, failed, completed);
	DPRINTF("last activity: %010ld.%06ld, errors: 0x%04"PRIx64", "
		"retries: 0x%04"PRIx64", received: 0x%08"PRIx64", "
		"returned: 0x%08"PRIx64", kicked: 0x%08"PRIx64"\n",
		vbd->ts.tv_sec, vbd->ts.tv_usec,
		vbd->errors, vbd->retries, vbd->received, vbd->returned,
		vbd->kicked);

	tapdisk_vbd_close_vdi(vbd);
	tapdisk_vbd_detach(vbd);
	tapdisk_server_remove_vbd(vbd);
	free(vbd->name);
	free(vbd);

	return 0;
}

int
tapdisk_vbd_close(td_vbd_t *vbd)
{
	/*
	 * don't close if any requests are pending in the aio layer
	 */
	if (!list_empty(&vbd->pending_requests))
		goto fail;

	/* 
	 * if the queue is still active and we have more
	 * requests, try to complete them before closing.
	 */
	if (tapdisk_vbd_queue_ready(vbd) &&
	    (!list_empty(&vbd->new_requests) ||
	     !list_empty(&vbd->failed_requests) ||
	     !list_empty(&vbd->completed_requests)))
		goto fail;

	return tapdisk_vbd_shutdown(vbd);

fail:
	td_flag_set(vbd->state, TD_VBD_SHUTDOWN_REQUESTED);
	DBG(TLOG_WARN, "%s: requests pending\n", vbd->name);
	return -EAGAIN;
}

/*
 * control operations
 */

void
tapdisk_vbd_debug(td_vbd_t *vbd)
{
	td_image_t *image, *tmp;
	int new, pending, failed, completed;

	tapdisk_vbd_queue_count(vbd, &new, &pending, &failed, &completed);

	DBG(TLOG_WARN, "%s: state: 0x%08x, new: 0x%02x, pending: 0x%02x, "
	    "failed: 0x%02x, completed: 0x%02x, last activity: %010ld.%06ld, "
	    "errors: 0x%04"PRIx64", retries: 0x%04"PRIx64", "
	    "received: 0x%08"PRIx64", returned: 0x%08"PRIx64", "
	    "kicked: 0x%08"PRIx64"\n",
	    vbd->name, vbd->state, new, pending, failed, completed,
	    vbd->ts.tv_sec, vbd->ts.tv_usec, vbd->errors, vbd->retries,
	    vbd->received, vbd->returned, vbd->kicked);

	tapdisk_vbd_for_each_image(vbd, image, tmp)
		td_debug(image);
}

static void
tapdisk_vbd_drop_log(td_vbd_t *vbd)
{
	if (td_flag_test(vbd->state, TD_VBD_LOG_DROPPED))
		return;

	tapdisk_vbd_debug(vbd);
	tlog_precious(0);
	td_flag_set(vbd->state, TD_VBD_LOG_DROPPED);
}

int
tapdisk_vbd_get_disk_info(td_vbd_t *vbd, td_disk_info_t *info)
{
	if (list_empty(&vbd->images))
		return -EINVAL;

	*info = tapdisk_vbd_first_image(vbd)->info;
	return 0;
}

static int
tapdisk_vbd_queue_ready(td_vbd_t *vbd)
{
	return (!td_flag_test(vbd->state, TD_VBD_DEAD) &&
		!td_flag_test(vbd->state, TD_VBD_CLOSED) &&
		!td_flag_test(vbd->state, TD_VBD_QUIESCED) &&
		!td_flag_test(vbd->state, TD_VBD_QUIESCE_REQUESTED));
}

int
tapdisk_vbd_retry_needed(td_vbd_t *vbd)
{
	return !(list_empty(&vbd->failed_requests) &&
		 list_empty(&vbd->new_requests));
}

int
tapdisk_vbd_lock(td_vbd_t *vbd)
{
	return 0;
}

int
tapdisk_vbd_quiesce_queue(td_vbd_t *vbd)
{
	if (!list_empty(&vbd->pending_requests)) {
		td_flag_set(vbd->state, TD_VBD_QUIESCE_REQUESTED);
		return -EAGAIN;
	}

	td_flag_clear(vbd->state, TD_VBD_QUIESCE_REQUESTED);
	td_flag_set(vbd->state, TD_VBD_QUIESCED);
	return 0;
}

int
tapdisk_vbd_start_queue(td_vbd_t *vbd)
{
	td_flag_clear(vbd->state, TD_VBD_QUIESCED);
	td_flag_clear(vbd->state, TD_VBD_QUIESCE_REQUESTED);
	tapdisk_vbd_mark_progress(vbd);
	return 0;
}

int
tapdisk_vbd_kill_queue(td_vbd_t *vbd)
{
	tapdisk_vbd_quiesce_queue(vbd);
	td_flag_set(vbd->state, TD_VBD_DEAD);
	return 0;
}

#if 0
static int
tapdisk_vbd_open_image(td_vbd_t *vbd, td_image_t *image)
{
	int err;
	td_image_t *parent;

	err = td_open(image);
	if (err)
		return err;

	if (!tapdisk_vbd_is_last_image(vbd, image)) {
		parent = tapdisk_vbd_next_image(image);
		err    = td_validate_parent(image, parent);
		if (err) {
			td_close(image);
			return err;
		}
	}

	return 0;
}
#endif

/*
 * Pausing a tapdisk can produce a lot of logging if the storage is not available
 * and there are inflight data requests. All the caller to squash the logging 
 * when entering a retry process.
 */
void tapdisk_vbd_squash_pause_logging(bool squash)
{
	log = !squash;
}

int
tapdisk_vbd_pause(td_vbd_t *vbd)
{
	int err;
	struct td_xenblkif *blkif;

	if (log) {
		INFO("pause requested\n");
	}

	td_flag_set(vbd->state, TD_VBD_PAUSE_REQUESTED);

	if (vbd->nbdserver)
		tapdisk_nbdserver_pause(vbd->nbdserver, log);

	err = tapdisk_vbd_quiesce_queue(vbd);
	if (err)
		return err;

	list_for_each_entry(blkif, &vbd->rings, entry)
		tapdisk_xenblkif_suspend(blkif);

	tapdisk_vbd_close_vdi(vbd);

	/* Don't guard this one as at this point the pause operation is complete */
	INFO("pause completed\n");

	if (!list_empty(&vbd->failed_requests))
		INFO("warning: failed requests pending\n");

	td_flag_clear(vbd->state, TD_VBD_PAUSE_REQUESTED);
	td_flag_set(vbd->state, TD_VBD_PAUSED);

	return 0;
}

int
tapdisk_vbd_resume(td_vbd_t *vbd, const char *name)
{
	int i, err;
    struct td_xenblkif *blkif;

	DBG(TLOG_DBG, "resume requested\n");

	if (!td_flag_test(vbd->state, TD_VBD_PAUSED)) {
		EPRINTF("resume request for unpaused vbd %s\n", vbd->name);
		return -EINVAL;
	}

	for (i = 0; i < TD_VBD_EIO_RETRIES; i++) {
		err = tapdisk_vbd_open_vdi(vbd, name, vbd->flags | TD_OPEN_STRICT, -1);
		if (!err)
			break;

		sleep(TD_VBD_EIO_SLEEP);
	}

	if (!err) {
		td_disk_info_t disk_info;
		err = tapdisk_vbd_get_disk_info(vbd, &disk_info);
		if (err) {
			EPRINTF("VBD %d failed to get disk info: %s\n", vbd->uuid,
					strerror(-err));
			goto resume_failed;
		}
		if (vbd->disk_info.size != disk_info.size
				|| vbd->disk_info.sector_size != disk_info.sector_size
				|| vbd->disk_info.info != disk_info.info) {
			EPRINTF("VBD %d cannot change disk info\n", vbd->uuid);
			err = -EMEDIUMTYPE;
			goto resume_failed;
		}
	}
resume_failed:
	if (err) {
		td_flag_set(vbd->state, TD_VBD_RESUME_FAILED);
		tapdisk_vbd_close_vdi(vbd);
		return err;
	}
	td_flag_clear(vbd->state, TD_VBD_RESUME_FAILED);

	DBG(TLOG_DBG, "resume completed\n");

	tapdisk_vbd_start_queue(vbd);
	td_flag_clear(vbd->state, TD_VBD_PAUSED);
	td_flag_clear(vbd->state, TD_VBD_PAUSE_REQUESTED);
	tapdisk_vbd_check_state(vbd);

	if (vbd->nbdserver)
		tapdisk_nbdserver_unpause(vbd->nbdserver);

    list_for_each_entry(blkif, &vbd->rings, entry)
		tapdisk_xenblkif_resume(blkif);


	DBG(TLOG_DBG, "state checked\n");

	return 0;
}

static int
tapdisk_vbd_request_ttl(td_vbd_request_t *vreq,
			const struct timeval *now)
{
	struct timeval delta;
	timersub(now, &vreq->ts, &delta);
	return vreq->vbd->req_timeout - delta.tv_sec;
}

static int
__tapdisk_vbd_request_timeout(td_vbd_request_t *vreq,
			      const struct timeval *now)
{
	int timeout;

	timeout = tapdisk_vbd_request_ttl(vreq, now) < 0;
	if (timeout)
		ERR(vreq->error,
		    "req %s timed out, retried %d times\n",
		    vreq->name, vreq->num_retries);

	return timeout;
}

static int
tapdisk_vbd_request_timeout(td_vbd_request_t *vreq)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	return __tapdisk_vbd_request_timeout(vreq, &now);
}

static void
tapdisk_vbd_check_queue_state(td_vbd_t *vbd)
{
	td_vbd_request_t *vreq, *tmp;
	struct timeval now;

	gettimeofday(&now, NULL);
	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->failed_requests)
		if (__tapdisk_vbd_request_timeout(vreq, &now))
			tapdisk_vbd_complete_vbd_request(vbd, vreq);

	if (!list_empty(&vbd->new_requests) ||
	    !list_empty(&vbd->failed_requests))
		tapdisk_vbd_issue_requests(vbd);

}

void
tapdisk_vbd_check_state(td_vbd_t *vbd)
{
	struct td_xenblkif *blkif;

	/*
	 * TODO don't ignore return value
	 */
	list_for_each_entry(blkif, &vbd->rings, entry)
		tapdisk_xenblkif_ring_stats_update(blkif);

	tapdisk_vbd_check_queue_state(vbd);

	if (td_flag_test(vbd->state, TD_VBD_QUIESCE_REQUESTED))
		tapdisk_vbd_quiesce_queue(vbd);

	if (td_flag_test(vbd->state, TD_VBD_PAUSE_REQUESTED))
		tapdisk_vbd_pause(vbd);

	if (td_flag_test(vbd->state, TD_VBD_SHUTDOWN_REQUESTED))
		tapdisk_vbd_close(vbd);
}

void watchdog_cleared(td_vbd_t *vbd)
{
	if (vbd->watchdog_warned) {
		DBG(TLOG_WARN, "%s: watchdog timeout: requests were blocked\n", vbd->name);
		/* Ideally want a direct way to flush the log */
		tlog_precious(1);
	}
	vbd->watchdog_warned = false;
}

void
tapdisk_vbd_check_progress(td_vbd_t *vbd)
{
	time_t diff;
	struct timeval now, delta;

	if (list_empty(&vbd->pending_requests)) {
		watchdog_cleared(vbd);
		return;
	}

	gettimeofday(&now, NULL);
	timersub(&now, &vbd->ts, &delta);
	diff = delta.tv_sec;

	if (diff >= TD_VBD_WATCHDOG_TIMEOUT)
	{
		if(tapdisk_vbd_queue_ready(vbd))
		{
			if (!vbd->watchdog_warned) {
				DBG(TLOG_WARN, "%s: watchdog timeout: pending requests "
				    "idle for %ld seconds\n", vbd->name, diff);
				vbd->watchdog_warned = true;
			}
			tapdisk_vbd_drop_log(vbd);
		}
		return;
	}

	watchdog_cleared(vbd);

	tapdisk_server_set_max_timeout(TD_VBD_WATCHDOG_TIMEOUT - diff);
}

/*
 * request submission 
 */

static int
tapdisk_vbd_check_queue(td_vbd_t *vbd)
{
	if (list_empty(&vbd->images))
		return -ENOSYS;

	if (!tapdisk_vbd_queue_ready(vbd))
		return -EAGAIN;

	return 0;
}

static int
tapdisk_vbd_request_should_retry(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	if (td_flag_test(vbd->state, TD_VBD_DEAD) ||
	    td_flag_test(vbd->state, TD_VBD_SHUTDOWN_REQUESTED))
		return 0;

	switch (abs(vreq->error)) {
	case EPERM:
	case ENOSYS:
	case ESTALE:
	case ENOSPC:
	case EFAULT:
		return 0;
	}

	if (tapdisk_vbd_request_timeout(vreq))
		return 0;

	return 1;
}

static void
tapdisk_vbd_complete_vbd_request(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	if (!vreq->submitting && !vreq->secs_pending) {
		if (vreq->error &&
		    tapdisk_vbd_request_should_retry(vbd, vreq))
			tapdisk_vbd_move_request(vreq, &vbd->failed_requests);
		else
			tapdisk_vbd_move_request(vreq, &vbd->completed_requests);
	}
}

static void
FIXME_maybe_count_enospc_redirect(td_vbd_t *vbd, td_request_t treq)
{
	int write = treq.op == TD_OP_WRITE;
	if (write &&
	    treq.image == tapdisk_vbd_first_image(vbd) &&
	    vbd->FIXME_enospc_redirect_count_enabled)
		vbd->FIXME_enospc_redirect_count += treq.secs;
}

static void
__tapdisk_vbd_complete_td_request(td_vbd_t *vbd, td_vbd_request_t *vreq,
				  td_request_t treq, int res)
{
	td_image_t *image = treq.image;
	int err;

        long long interval;

	err = (res <= 0 ? res : -res);
	vbd->secs_pending  -= treq.secs;
	vreq->secs_pending -= treq.secs;

	if (err != -EBUSY) {
		int write = treq.op == TD_OP_WRITE;
		td_sector_count_add(&image->stats.hits, treq.secs, write);
		if (err)
			td_sector_count_add(&image->stats.fail,
					    treq.secs, write);

		FIXME_maybe_count_enospc_redirect(vbd, treq);
	}

	if (err) {
		if (err != -EBUSY) {
			if (!vreq->error &&
			    err != vreq->prev_error)
				tlog_drv_error(image->driver, err,
					       "req %s: %s 0x%04x secs @ 0x%08"PRIx64" - %s",
					       vreq->name,
					       op_strings[treq.op],
					       treq.secs, treq.sec, strerror(abs(err)));
			vbd->errors++;
		}
		vreq->error = (vreq->error ? : err);
	}

        interval = timeval_to_us(&vbd->ts) - timeval_to_us(&vreq->ts);

        if(treq.op == TD_OP_READ) {
            vbd->vdi_stats.stats->read_reqs_completed++;
            vbd->vdi_stats.stats->read_sectors += treq.secs;
            vbd->vdi_stats.stats->read_total_ticks += interval;
        }

        if(treq.op == TD_OP_WRITE) {
            vbd->vdi_stats.stats->write_reqs_completed++;
            vbd->vdi_stats.stats->write_sectors += treq.secs;
            vbd->vdi_stats.stats->write_total_ticks += interval;
        }

	tapdisk_vbd_complete_vbd_request(vbd, vreq);
}

static void
__tapdisk_vbd_reissue_td_request(td_vbd_t *vbd,
				 td_image_t *image, td_request_t treq)
{
	td_image_t *parent;
	td_vbd_request_t *vreq;

	vreq = treq.vreq;
	gettimeofday(&vreq->last_try, NULL);

	vreq->submitting++;

	if (tapdisk_vbd_is_last_image(vbd, image)) {
		if (unlikely(treq.op == TD_OP_BLOCK_STATUS)) {
			treq.status = TD_BLOCK_STATE_HOLE;
		} else {
			memset(treq.buf, 0, (size_t)treq.secs << SECTOR_SHIFT);
		}
		td_complete_request(treq, 0);
		goto done;
	}
	/*
	 * If intellicache is enabled, check to confirm  mirroring
	 * is disabled due to out of space.
	 */
	if (unlikely(vbd->retired && vbd->retired == image))
		parent = tapdisk_vbd_first_image(vbd);
	else
		parent = tapdisk_vbd_next_image(image);

	treq.image = parent;

	/* return zeros for requests that extend beyond end of parent image */
	if (treq.sec + treq.secs > parent->info.size) {
		td_request_t clone  = treq;

		if (parent->info.size > treq.sec) {
			int secs    = parent->info.size - treq.sec;
			clone.sec  += secs;
			clone.secs -= secs;
			clone.buf  += (secs << SECTOR_SHIFT);
			treq.secs   = secs;
		} else
			treq.secs   = 0;

		memset(clone.buf, 0, (size_t)clone.secs << SECTOR_SHIFT);
		td_complete_request(clone, 0);

		if (!treq.secs)
			goto done;
	}

	switch (treq.op) {
	case TD_OP_WRITE:
		td_queue_write(parent, treq);
		break;

	case TD_OP_READ:
		td_queue_read(parent, treq);
		break;
	case TD_OP_BLOCK_STATUS:
		td_queue_block_status(parent, &treq);
		break;
	}

done:
	vreq->submitting--;
	if (!vreq->secs_pending)
		tapdisk_vbd_complete_vbd_request(vbd, vreq);
}

void
tapdisk_vbd_forward_request(td_request_t treq)
{
	td_vbd_t *vbd;
	td_image_t *image;
	td_vbd_request_t *vreq;

	image = treq.image;
	vreq  = treq.vreq;
	vbd   = vreq->vbd;

	tapdisk_vbd_mark_progress(vbd);

	if (tapdisk_vbd_queue_ready(vbd))
		__tapdisk_vbd_reissue_td_request(vbd, image, treq);
	else
		__tapdisk_vbd_complete_td_request(vbd, vreq, treq, -EBUSY);
}

int
add_extent(tapdisk_extents_t *extents, td_request_t *vreq)
{
	tapdisk_extent_t *extent;
	extent  = (tapdisk_extent_t*)malloc(sizeof(*extent));
	if(extent == NULL)
		return -1;

	extent->start = vreq->sec;
	extent->length = vreq->secs;
	extent->flag = vreq->status;
	extent->next = NULL;

	if(extents->head != NULL && extents->tail != NULL) {
		extents->tail->next = extent;
		extents->tail = extent;
	} else {
		extents->tail = extent;
		extents->head = extent;
	}

	extents->count += 1;
	return 0;
}

int
block_status_add_extent(tapdisk_extents_t *extents, td_request_t *vreq)
{
	int ret  = 0;
	if(extents->tail == NULL) {
		ret = add_extent(extents, vreq);
	} else {
		if(extents->tail->flag == vreq->status) {
			extents->tail->length += vreq->secs;
		} else {
			ret = add_extent(extents, vreq);
		}
	}
	return ret;
}

void
tapdisk_vbd_complete_block_status_request(td_request_t treq, int res)
{
	td_vbd_t *vbd;
	td_image_t *image;
	td_vbd_request_t *vreq;

	image = treq.image;
	vreq  = treq.vreq;
	vbd   = vreq->vbd;
	tapdisk_vbd_mark_progress(vbd);

	/* Record this extents in the vreqs data */
	tapdisk_extents_t* extents = (tapdisk_extents_t*)vreq->data;
	if( block_status_add_extent(extents, &treq) != 0) {
		ERROR("Could not allocate extent structure");
		/* Propagate the ENOMEM */
		res = -ENOMEM;
	}

	DBG(TLOG_DBG, "%s: req %s seg %d sec 0x%08"PRIx64
	    " secs 0x%04x buf %p op %d res %d\n", image->name,
	    vreq->name, treq.sidx, treq.sec, treq.secs,
	    treq.buf, vreq->op, res);

	__tapdisk_vbd_complete_td_request(vbd, vreq, treq, res);
}

void
tapdisk_vbd_complete_td_request(td_request_t treq, int res)
{
	td_vbd_t *vbd;
	td_image_t *image, *leaf;
	td_vbd_request_t *vreq;

	image = treq.image;
	vreq  = treq.vreq;
	vbd   = vreq->vbd;

	tapdisk_vbd_mark_progress(vbd);

	if (abs(res) == ENOSPC && td_flag_test(image->flags,
				TD_IGNORE_ENOSPC)) {
		res = 0;
		leaf = tapdisk_vbd_first_image(vbd);
		if (vbd->secondary_mode == TD_VBD_SECONDARY_MIRROR) {
			DPRINTF("ENOSPC: disabling mirroring\n");
			list_del_init(&leaf->next);
			vbd->retired = leaf;
		} else if (vbd->secondary_mode == TD_VBD_SECONDARY_STANDBY) {
			DPRINTF("ENOSPC: failing over to secondary image\n");
			list_add(&vbd->secondary->next, leaf->next.prev);
			vbd->FIXME_enospc_redirect_count_enabled = 1;
		}
		if (vbd->secondary_mode != TD_VBD_SECONDARY_DISABLED) {
			vbd->secondary = NULL;
			vbd->secondary_mode = TD_VBD_SECONDARY_DISABLED;
			signal_enospc(vbd);
		}
	}

	if (res != 0 && image->type == DISK_TYPE_NBD && 
			((image == vbd->secondary) || 
			 (image == vbd->retired))) {
		ERROR("Got non-zero res %d for NBD secondary - disabling "
		      "mirroring: %s", res, vreq->name);
		vbd->nbd_mirror_failed = 1;
		res = 0; /* Pretend the writes have completed successfully */

		/* It was the secondary that timed out - disable secondary */
		list_del_init(&image->next);
		vbd->retired = image;
		if (vbd->secondary_mode != TD_VBD_SECONDARY_DISABLED) {
			vbd->secondary = NULL;
			vbd->secondary_mode = TD_VBD_SECONDARY_DISABLED;
		}
	}

	DBG(TLOG_DBG, "%s: req %s seg %d sec 0x%08"PRIx64
	    " secs 0x%04x buf %p op %d res %d\n", image->name,
	    vreq->name, treq.sidx, treq.sec, treq.secs,
	    treq.buf, vreq->op, res);

	__tapdisk_vbd_complete_td_request(vbd, vreq, treq, res);
}

static inline void
queue_mirror_req(td_vbd_t *vbd, td_request_t clone)
{
	clone.image = vbd->secondary;
	td_queue_write(vbd->secondary, clone);
}

int
tapdisk_vbd_issue_request(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	td_image_t *image;
	td_request_t treq;
	bzero(&treq, sizeof(treq));
	td_sector_t sec;
	int i, err;

	sec    = vreq->sec;
	image  = tapdisk_vbd_first_image(vbd);

	vreq->submitting = 1;

	tapdisk_vbd_mark_progress(vbd);
	vreq->last_try = vbd->ts;

	tapdisk_vbd_move_request(vreq, &vbd->pending_requests);

	err = tapdisk_vbd_check_queue(vbd);
	if (err) {
		vreq->error = err;
		goto fail;
	}

	err = tapdisk_image_check_request(image, vreq);
	if (err) {
		vreq->error = err;
		goto fail;
	}

	for (i = 0; i < vreq->iovcnt; i++) {
		struct td_iovec *iov = &vreq->iov[i];

		treq.sidx           = i;
		treq.buf            = iov->base;
		treq.sec            = sec;
		treq.secs           = iov->secs;
		treq.image          = image;
		treq.cb             = tapdisk_vbd_complete_td_request;
		treq.cb_data        = NULL;
		treq.vreq           = vreq;


		vreq->secs_pending += iov->secs;
		vbd->secs_pending  += iov->secs;
		if (vbd->secondary_mode == TD_VBD_SECONDARY_MIRROR &&
		    vreq->op == TD_OP_WRITE) {
			vreq->secs_pending += iov->secs;
			vbd->secs_pending  += iov->secs;
		}

		switch (vreq->op) {
		case TD_OP_WRITE:
			treq.op = TD_OP_WRITE;
                        vbd->vdi_stats.stats->write_reqs_submitted++;
			/*
			 * it's important to queue the mirror request before 
			 * queuing the main one. If the main image runs into 
			 * ENOSPC, the mirroring could be disabled before 
			 * td_queue_write returns, so if the mirror request was 
			 * queued after (which would then not happen), we'd 
			 * lose that write and cause the process to hang with 
			 * unacknowledged writes
			 */
			if (vbd->secondary_mode == TD_VBD_SECONDARY_MIRROR)
				queue_mirror_req(vbd, treq);
			td_queue_write(treq.image, treq);
			break;

		case TD_OP_READ:
			treq.op = TD_OP_READ;
                        vbd->vdi_stats.stats->read_reqs_submitted++;
			td_queue_read(treq.image, treq);
			break;
		case TD_OP_BLOCK_STATUS:
			treq.op = TD_OP_BLOCK_STATUS;
			treq.cb = tapdisk_vbd_complete_block_status_request;
			td_queue_block_status(treq.image, &treq);
			break;
		}

		DBG(TLOG_DBG, "%s: req %s seg %d sec 0x%08"PRIx64" secs 0x%04x "
		    "buf %p op %d\n", image->name, vreq->name, i, treq.sec, treq.secs,
		    treq.buf, vreq->op);
		sec += iov->secs;
	}

	err = 0;

out:
	vreq->submitting--;
	if (!vreq->secs_pending) {
		err = (err ? : vreq->error);
		tapdisk_vbd_complete_vbd_request(vbd, vreq);
	}

	return err;

fail:
	vreq->error = err;
	goto out;
}

static int
tapdisk_vbd_request_completed(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	return vreq->list_head == &vbd->completed_requests;
}

static int
tapdisk_vbd_reissue_failed_requests(td_vbd_t *vbd)
{
	int err;
	struct timeval now;
	td_vbd_request_t *vreq, *tmp;

	err = 0;
	gettimeofday(&now, NULL);

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->failed_requests) {
		if (vreq->secs_pending)
			continue;

		if (td_flag_test(vbd->state, TD_VBD_SHUTDOWN_REQUESTED)) {
			tapdisk_vbd_complete_vbd_request(vbd, vreq);
			continue;
		}

		if (vreq->error != -EBUSY &&
		    now.tv_sec - vreq->last_try.tv_sec < TD_VBD_RETRY_INTERVAL)
			continue;

		vbd->retries++;
		vreq->num_retries++;

		vreq->prev_error = vreq->error;
		vreq->error      = 0;

		DBG(TLOG_DBG, "retry #%d of req %s, "
		    "sec 0x%08"PRIx64", iovcnt: %d\n", vreq->num_retries,
		    vreq->name, vreq->sec, vreq->iovcnt);

		err = tapdisk_vbd_issue_request(vbd, vreq);
		/*
		 * if this request failed, but was not completed,
		 * we'll back off for a while.
		 */
		if (err && !tapdisk_vbd_request_completed(vbd, vreq))
			break;
	}

	return 0;
}

static void
tapdisk_vbd_count_new_request(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	struct td_iovec *iov;
	int write;

	write = vreq->op == TD_OP_WRITE;

	for (iov = &vreq->iov[0]; iov < &vreq->iov[vreq->iovcnt]; iov++)
		td_sector_count_add(&vbd->secs, iov->secs, write);
}

static int
tapdisk_vbd_issue_new_requests(td_vbd_t *vbd)
{
	int err;
	td_vbd_request_t *vreq, *tmp;

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->new_requests) {
		err = tapdisk_vbd_issue_request(vbd, vreq);
		/*
		 * if this request failed, but was not completed,
		 * we'll back off for a while.
		 */
		if (err && !tapdisk_vbd_request_completed(vbd, vreq))
			return err;

		tapdisk_vbd_count_new_request(vbd, vreq);
	}

	return 0;
}

int
tapdisk_vbd_recheck_state(td_vbd_t *vbd)
{
	if (list_empty(&vbd->new_requests))
		return 0;

	if (td_flag_test(vbd->state, TD_VBD_QUIESCED) ||
	    td_flag_test(vbd->state, TD_VBD_QUIESCE_REQUESTED))
		return 0;

	tapdisk_vbd_issue_new_requests(vbd);

	return 1;
}

static int
tapdisk_vbd_kill_requests(td_vbd_t *vbd)
{
	td_vbd_request_t *vreq, *tmp;

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->new_requests) {
		vreq->error = -ESHUTDOWN;
		tapdisk_vbd_move_request(vreq, &vbd->completed_requests);
	}

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->failed_requests) {
		vreq->error = -ESHUTDOWN;
		tapdisk_vbd_move_request(vreq, &vbd->completed_requests);
	}

	return 0;
}

int
tapdisk_vbd_issue_requests(td_vbd_t *vbd)
{
	int err;

	if (td_flag_test(vbd->state, TD_VBD_DEAD))
		return tapdisk_vbd_kill_requests(vbd);

	if (td_flag_test(vbd->state, TD_VBD_QUIESCED) ||
	    td_flag_test(vbd->state, TD_VBD_QUIESCE_REQUESTED)) {

		if (td_flag_test(vbd->state, TD_VBD_RESUME_FAILED))
			return tapdisk_vbd_kill_requests(vbd);
		else
			return -EAGAIN;
	}

	err = tapdisk_vbd_reissue_failed_requests(vbd);
	if (err)
		return err;

	return tapdisk_vbd_issue_new_requests(vbd);
}

int
tapdisk_vbd_queue_request(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	gettimeofday(&vreq->ts, NULL);
	vreq->vbd = vbd;

	list_add_tail(&vreq->next, &vbd->new_requests);
	vbd->received++;

	return 0;
}

void
tapdisk_vbd_kick(td_vbd_t *vbd)
{
	const struct list_head *list = &vbd->completed_requests;
	td_vbd_request_t *vreq, *prev, *next;

	vbd->kicked++;

	while (!list_empty(list)) {

		/*
		 * Take one request off the completed requests list, and then look for
		 * other requests in the same list that have the same token and
		 * complete them. This way we complete requests against the same token
		 * in one go before we proceed to completing requests with other
		 * tokens. The token is usually used to point back to some other
		 * structure, e.g. a blktap or a tapdisk3 connexion. Once all requests
		 * with a specific token have been completed, proceed to the next one
		 * until the list is empty.
		 */
		prev = list_entry(list->next, td_vbd_request_t, next);
		list_del(&prev->next);

		tapdisk_vbd_for_each_request(vreq, next, list) {
			if (vreq->token == prev->token) {

				prev->cb(prev, prev->error, prev->token, 0);
				vbd->returned++;

				list_del(&vreq->next);
				prev = vreq;
			}
		}

		prev->cb(prev, prev->error, prev->token, 1);
		vbd->returned++;
	}
}

int
tapdisk_vbd_start_nbdserver(td_vbd_t *vbd)
{
	td_disk_info_t info;
	int err;

	err = tapdisk_vbd_get_disk_info(vbd, &info);

	if (err)
		return err;

	vbd->nbdserver = tapdisk_nbdserver_alloc(vbd, info);

	if (!vbd->nbdserver) {
		EPRINTF("Error starting nbd server");
		return -1;
	}

	err = tapdisk_nbdserver_listen_unix(vbd->nbdserver);
	if (err) {
		tapdisk_nbdserver_free(vbd->nbdserver);
		EPRINTF("failed to listen on the UNIX domain socket: %s\n",
				strerror(-err));
		return err;
	}

	return 0;
}


static int
tapdisk_vbd_reqs_outstanding(td_vbd_t *vbd)
{
	int new, pending, failed, completed;

	ASSERT(vbd);

	tapdisk_vbd_queue_count(vbd, &new, &pending, &failed, &completed);

	return new + pending + failed + completed;
}


void
tapdisk_vbd_stats(td_vbd_t *vbd, td_stats_t *st)
{
	td_image_t *image, *next;
    struct td_xenblkif *blkif;
	const bool read_caching =
		TD_OPEN_NO_O_DIRECT == (vbd->flags & TD_OPEN_NO_O_DIRECT);

	tapdisk_stats_enter(st, '{');
	tapdisk_stats_field(st, "name", "s", vbd->name);

	tapdisk_stats_field(st, "secs", "[");
	tapdisk_stats_val(st, "llu", vbd->secs.rd);
	tapdisk_stats_val(st, "llu", vbd->secs.wr);
	tapdisk_stats_leave(st, ']');

	tapdisk_stats_field(st, "images", "[");
	tapdisk_vbd_for_each_image(vbd, image, next)
		tapdisk_image_stats(image, st);
	tapdisk_stats_leave(st, ']');

	if (vbd->tap) {
		tapdisk_stats_field(st, "tap", "{");
		tapdisk_blktap_stats(vbd->tap, st);
		tapdisk_stats_leave(st, '}');
	}

    /*
     * TODO Is this used by any one?
     */
    if (!list_empty(&vbd->rings)) {
	    tapdisk_stats_field(st, "xenbus", "{");
        list_for_each_entry(blkif, &vbd->rings, entry)
		    tapdisk_xenblkif_stats(blkif, st);
    	tapdisk_stats_leave(st, '}');
    }

	tapdisk_stats_field(st,
			"FIXME_enospc_redirect_count",
			"llu", vbd->FIXME_enospc_redirect_count);

	tapdisk_stats_field(st,
			"nbd_mirror_failed",
			"d", vbd->nbd_mirror_failed);

	tapdisk_stats_field(st,
			"reqs_outstanding",
			"d", tapdisk_vbd_reqs_outstanding(vbd));

	tapdisk_stats_field(st,
			"read_caching",
			"s",  read_caching ? "true": "false");

	tapdisk_stats_leave(st, '}');
}


bool
tapdisk_vbd_contains_dead_rings(td_vbd_t * vbd)
{
    return !list_empty(&vbd->dead_rings);
}
