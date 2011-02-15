/*
 * Copyright (c) 2008, XenSource Inc.
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <unistd.h>
#include <stdlib.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "blktap.h"
#include "libvhd.h"
#include "tapdisk-image.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-vbd.h"
#include "tapdisk-disktype.h"
#include "tapdisk-interface.h"
#include "tapdisk-stats.h"
#include "tapdisk-storage.h"

#define DBG(_level, _f, _a...) tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...) tlog_error(_err, _f, ##_a)

#if 1                                                                        
#define ASSERT(p)							\
	do {								\
		if (!(p)) {						\
			DPRINTF("Assertion '%s' failed, line %d, "	\
				"file %s", #p, __LINE__, __FILE__);	\
			*(int*)0 = 0;					\
		}							\
	} while (0)
#else
#define ASSERT(p) ((void)0)
#endif 


#define TD_VBD_EIO_RETRIES          10
#define TD_VBD_EIO_SLEEP            1
#define TD_VBD_WATCHDOG_TIMEOUT     10

static void tapdisk_vbd_ring_event(event_id_t, char, void *);
static void tapdisk_vbd_complete_vbd_request(td_vbd_t *, td_vbd_request_t *);
static void tapdisk_vbd_callback(void *, blkif_response_t *);
static int  tapdisk_vbd_queue_ready(td_vbd_t *);
static void tapdisk_vbd_check_queue_state(td_vbd_t *);

/* 
 * initialization
 */

static inline void
tapdisk_vbd_initialize_vreq(td_vbd_request_t *vreq)
{
	memset(vreq, 0, sizeof(td_vbd_request_t));
	INIT_LIST_HEAD(&vreq->next);
}

static void
tapdisk_vbd_mark_progress(td_vbd_t *vbd)
{
	gettimeofday(&vbd->ts, NULL);
}

td_vbd_t*
tapdisk_vbd_create(uint16_t uuid)
{
	td_vbd_t *vbd;
	int i;

	vbd = calloc(1, sizeof(td_vbd_t));
	if (!vbd) {
		EPRINTF("failed to allocate tapdisk state\n");
		return NULL;
	}

	vbd->uuid     = uuid;
	vbd->minor    = -1;
	vbd->ring.fd  = -1;

	/* default blktap ring completion */
	vbd->callback = tapdisk_vbd_callback;
	vbd->argument = vbd;

	INIT_LIST_HEAD(&vbd->images);
	INIT_LIST_HEAD(&vbd->new_requests);
	INIT_LIST_HEAD(&vbd->pending_requests);
	INIT_LIST_HEAD(&vbd->failed_requests);
	INIT_LIST_HEAD(&vbd->completed_requests);
	INIT_LIST_HEAD(&vbd->next);
	tapdisk_vbd_mark_progress(vbd);

	for (i = 0; i < MAX_REQUESTS; i++)
		tapdisk_vbd_initialize_vreq(vbd->request_list + i);

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

	tapdisk_server_add_vbd(vbd);

	return 0;
}

void
tapdisk_vbd_set_callback(td_vbd_t *vbd, td_vbd_cb_t callback, void *argument)
{
	vbd->callback = callback;
	vbd->argument = argument;
}

static int
tapdisk_vbd_validate_chain(td_vbd_t *vbd)
{
	int err;
	td_image_t *image, *parent, *tmp;

	DPRINTF("VBD CHAIN:\n");

	tapdisk_vbd_for_each_image(vbd, image, tmp) {
		DPRINTF("%s: type:%s(%d) storage:%s(%d)\n",
			image->name,
			tapdisk_disk_types[image->type]->name,
			image->type,
			tapdisk_storage_name(image->driver->storage),
			image->driver->storage);

		if (tapdisk_vbd_is_last_image(vbd, image))
			break;

		parent = tapdisk_vbd_next_image(image);
		err    = td_validate_parent(image, parent);
		if (err)
			return err;
	}

	return 0;
}

void
tapdisk_vbd_close_vdi(td_vbd_t *vbd)
{
	td_image_t *image, *tmp;

	tapdisk_vbd_for_each_image(vbd, image, tmp) {
		td_close(image);
		tapdisk_image_free(image);
	}

	if (vbd->secondary && vbd->secondary_mode != TD_VBD_SECONDARY_MIRROR) {
		/* in mirror mode the image will have been closed as part of 
		 * the chain */
		td_close(vbd->secondary);
		tapdisk_image_free(vbd->secondary);
		DPRINTF("Secondary image closed\n");
	}

	if (vbd->retired) {
		td_close(vbd->retired);
		tapdisk_image_free(vbd->retired);
		DPRINTF("Retired mirror image closed\n");
	}

	INIT_LIST_HEAD(&vbd->images);
	td_flag_set(vbd->state, TD_VBD_CLOSED);
}

static int
tapdisk_vbd_add_block_cache(td_vbd_t *vbd)
{
	int err;
	td_driver_t *driver;
	td_image_t *cache, *image, *target, *tmp;

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
	err = td_open(cache);
	if (!err)
		goto done;

fail:
	/* give up */
	tapdisk_image_free(target);
	return err;

done:
	/* insert cache before image */
	list_add(&cache->next, target->next.prev);
	return 0;
}

static int
tapdisk_vbd_add_local_cache(td_vbd_t *vbd)
{
	int err;
	td_driver_t *driver;
	td_image_t *cache, *parent;

	parent = tapdisk_vbd_first_image(vbd);
	if (tapdisk_vbd_is_last_image(vbd, parent)) {
		DPRINTF("Single-image chain, nothing to cache");
		return 0;
	}

	cache = tapdisk_image_allocate(parent->name,
				       DISK_TYPE_LOCAL_CACHE,
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
	err = td_open(cache);
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

static int
tapdisk_vbd_add_secondary(td_vbd_t *vbd)
{
	int err;
	td_driver_t *driver;
	td_image_t *leaf, *second;

	DPRINTF("Adding secondary image: %s\n", vbd->secondary_name);

	leaf = tapdisk_vbd_first_image(vbd);
	second = tapdisk_image_allocate(vbd->secondary_name,
					vbd->secondary_type,
					leaf->flags);

	if (!second)
		return -ENOMEM;

	second->driver = tapdisk_driver_allocate(second->type,
						 second->name,
						 second->flags);

	if (!second->driver) {
		err = -ENOMEM;
		goto fail;
	}

	second->driver->info = leaf->driver->info;

	/* try to open the secondary image */
	err = td_open(second);
	if (err)
		goto fail;

	if (second->info.size != leaf->info.size) {
		EPRINTF("Secondary image size %lld != image size %lld\n",
				second->info.size, leaf->info.size);
		err = -EINVAL;
		goto fail;
	}

	goto done;

fail:
	tapdisk_image_free(second);
	return err;

done:
	vbd->secondary = second;
	leaf->flags |= TD_IGNORE_ENOSPC;
	if (td_flag_test(vbd->flags, TD_OPEN_STANDBY)) {
		DPRINTF("In standby mode\n");
		vbd->secondary_mode = TD_VBD_SECONDARY_STANDBY;
	} else {
		DPRINTF("In mirror mode\n");
		vbd->secondary_mode = TD_VBD_SECONDARY_MIRROR;
		/* we actually need this image to also be part of the chain, 
		 * since it may already contain data */
		list_add(&vbd->secondary->next, &leaf->next);
	}

	DPRINTF("Added secondary image\n");
	return 0;
}

static void signal_enospc(td_vbd_t *vbd)
{
	int fd, err;
	char *fn;

	err = asprintf(&fn, BLKTAP2_ENOSPC_SIGNAL_FILE"%d", vbd->minor);
	if (err == -1) {
		EPRINTF("Failed to signal ENOSPC condition\n");
		return;
	}

	fd = open(fn, O_WRONLY | O_CREAT | O_NONBLOCK);
	if (fd == -1)
		EPRINTF("Failed to open file to signal ENOSPC condition\n");
	else
		close(fd);

	free(fn);
}

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

static int
tapdisk_vbd_add_dirty_log(td_vbd_t *vbd)
{
	int err;
	td_driver_t *driver;
	td_image_t *log, *parent;

	driver = NULL;
	log    = NULL;

	parent = tapdisk_vbd_first_image(vbd);

	log    = tapdisk_image_allocate(parent->name,
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

	err = td_open(log);
	if (err)
		goto fail;

	tapdisk_vbd_add_image(vbd, log);
	return 0;

fail:
	tapdisk_image_free(log);
	return err;
}

static int
__tapdisk_vbd_open_vdi(td_vbd_t *vbd, td_flag_t extra_flags)
{
	int err, type;
	td_disk_id_t id;
	td_image_t *image, *tmp;
	struct tfilter *filter = NULL;

	id.flags = (vbd->flags & ~TD_OPEN_SHAREABLE) | extra_flags;
	id.name  = vbd->name;
	id.type  = vbd->type;

	for (;;) {
		err   = -ENOMEM;
		image = tapdisk_image_allocate(id.name, id.type, id.flags);

		if (id.name != vbd->name) {
			free(id.name);
			id.name = NULL;
		}

		if (!image)
			goto fail;

		err = td_load(image);
		if (err) {
			if (err != -ENODEV)
				goto fail;

			if (td_flag_test(id.flags, TD_OPEN_VHD_INDEX) &&
			    td_flag_test(id.flags, TD_OPEN_RDONLY)) {
				err = tapdisk_vbd_open_index(vbd);
				if (!err) {
					tapdisk_image_free(image);
					image = NULL;
					break;
				}

				if (err != -ENOENT)
					goto fail;
			}

			err = td_open(image);
			if (err)
				goto fail;
		}

		err = td_get_parent_id(image, &id);
		if (err && err != TD_NO_PARENT) {
			td_close(image);
			goto fail;
		}

		tapdisk_vbd_add_image(vbd, image);
		image = NULL;

		if (err == TD_NO_PARENT)
			break;

		if (id.flags & TD_OPEN_REUSE_PARENT) {
			free(id.name);
			err = asprintf(&id.name, "%s%d", BLKTAP2_IO_DEVICE,
				       vbd->parent_devnum);
			if (err == -1) {
				err = ENOMEM;
				goto fail;
			}
			type = DISK_TYPE_AIO;
		}
	}

	if (td_flag_test(vbd->flags, TD_OPEN_LOG_DIRTY)) {
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
		if (err)
			goto fail;
	}

	td_flag_clear(vbd->state, TD_VBD_CLOSED);

	return 0;

fail:
	if (image)
		tapdisk_image_free(image);

	tapdisk_vbd_close_vdi(vbd);

	return err;
}

int
tapdisk_vbd_open_vdi(td_vbd_t *vbd, int type, const char *path,
		     td_flag_t flags, int prt_devnum,
		     int secondary_type, const char *secondary_name)
{
	const disk_info_t *info;
	int i, err;

	info = tapdisk_disk_types[type];
	if (!info)
		return -EINVAL;

	DPRINTF("Loading driver '%s' for vbd %u %s 0x%08x\n",
		info->name, vbd->uuid, path, flags);

	err = tapdisk_namedup(&vbd->name, path);
	if (err)
		return err;

	if (flags & TD_OPEN_REUSE_PARENT)
		vbd->parent_devnum = prt_devnum;

	if (flags & TD_OPEN_SECONDARY) {
		vbd->secondary_type = secondary_type;
		err = tapdisk_namedup(&vbd->secondary_name, secondary_name);
		if (err)
			return err;
	}

	vbd->flags   = flags;
	vbd->type    = type;

	err = __tapdisk_vbd_open_vdi(vbd, 0);
	if (err)
		goto fail;

	return 0;

fail:
	free(vbd->name);
	vbd->name = NULL;
	return err;
}

static int
tapdisk_vbd_register_event_watches(td_vbd_t *vbd)
{
	event_id_t id;

	id = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					   vbd->ring.fd, 0,
					   tapdisk_vbd_ring_event, vbd);
	if (id < 0)
		return id;

	vbd->ring_event_id = id;

	return 0;
}

static void
tapdisk_vbd_unregister_events(td_vbd_t *vbd)
{
	if (vbd->ring_event_id)
		tapdisk_server_unregister_event(vbd->ring_event_id);
}

static int
tapdisk_vbd_map_device(td_vbd_t *vbd, const char *devname)
{
	
	int err, psize;
	td_ring_t *ring;

	ring  = &vbd->ring;
	psize = getpagesize();

	ring->fd = open(devname, O_RDWR);
	if (ring->fd == -1) {
		err = -errno;
		EPRINTF("failed to open %s: %d\n", devname, err);
		goto fail;
	}

	ring->mem = mmap(0, psize * BLKTAP_MMAP_REGION_SIZE,
			 PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, 0);
	if (ring->mem == MAP_FAILED) {
		err = -errno;
		EPRINTF("failed to mmap %s: %d\n", devname, err);
		goto fail;
	}

	ring->sring = (blkif_sring_t *)((unsigned long)ring->mem);
	BACK_RING_INIT(&ring->fe_ring, ring->sring, psize);

	ring->vstart =
		(unsigned long)ring->mem + (BLKTAP_RING_PAGES * psize);

	return 0;

fail:
	if (ring->mem && ring->mem != MAP_FAILED)
		munmap(ring->mem, psize * BLKTAP_MMAP_REGION_SIZE);
	if (ring->fd != -1)
		close(ring->fd);
	ring->fd  = -1;
	ring->mem = NULL;
	return err;
}

static int
tapdisk_vbd_unmap_device(td_vbd_t *vbd)
{
	int psize;

	psize = getpagesize();

	if (vbd->ring.fd != -1)
		close(vbd->ring.fd);
	if (vbd->ring.mem > 0)
		munmap(vbd->ring.mem, psize * BLKTAP_MMAP_REGION_SIZE);

	return 0;
}

void
tapdisk_vbd_detach(td_vbd_t *vbd)
{
	tapdisk_vbd_unregister_events(vbd);

	tapdisk_vbd_unmap_device(vbd);
	vbd->minor = -1;
}


int
tapdisk_vbd_attach(td_vbd_t *vbd, const char *devname, int minor)
{
	int err;

	err = tapdisk_vbd_map_device(vbd, devname);
	if (err)
		goto fail;

	err = tapdisk_vbd_register_event_watches(vbd);
	if (err)
		goto fail;

	vbd->minor = minor;

	return 0;

fail:
	tapdisk_vbd_detach(vbd);

	return err;
}

int
tapdisk_vbd_open(td_vbd_t *vbd, int type, const char *path,
		 int minor, const char *ring, td_flag_t flags)
{
	int err;

	err = tapdisk_vbd_open_vdi(vbd, type, path, flags, -1, -1, NULL);
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

	tapdisk_vbd_kick(vbd);
	tapdisk_vbd_queue_count(vbd, &new, &pending, &failed, &completed);

	DPRINTF("%s: state: 0x%08x, new: 0x%02x, pending: 0x%02x, "
		"failed: 0x%02x, completed: 0x%02x\n", 
		vbd->name, vbd->state, new, pending, failed, completed);
	DPRINTF("last activity: %010ld.%06ld, errors: 0x%04"PRIx64", "
		"retries: 0x%04"PRIx64", received: 0x%08"PRIx64", "
		"returned: 0x%08"PRIx64", kicked: 0x%08"PRIx64", "
		"kicks in: 0x%08"PRIx64", out: 0x%08"PRIu64"\n",
		vbd->ts.tv_sec, vbd->ts.tv_usec,
		vbd->errors, vbd->retries, vbd->received, vbd->returned,
		vbd->kicked, vbd->kicks_in, vbd->kicks_out);

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
	    "errors: 0x%04llx, retries: 0x%04llx, received: 0x%08llx, "
	    "returned: 0x%08llx, kicked: 0x%08llx, "
	    "kicks in: 0x%08"PRIx64", out: 0x%08"PRIx64"\n",
	    vbd->name, vbd->state, new, pending, failed, completed,
	    vbd->ts.tv_sec, vbd->ts.tv_usec, vbd->errors, vbd->retries,
	    vbd->received, vbd->returned, vbd->kicked,
	    vbd->kicks_in, vbd->kicks_out);

	tapdisk_vbd_for_each_image(vbd, image, tmp)
		td_debug(image);
}

static void
tapdisk_vbd_drop_log(td_vbd_t *vbd)
{
	if (td_flag_test(vbd->state, TD_VBD_LOG_DROPPED))
		return;

	tapdisk_vbd_debug(vbd);
	tlog_precious();
	td_flag_set(vbd->state, TD_VBD_LOG_DROPPED);
}

int
tapdisk_vbd_get_disk_info(td_vbd_t *vbd, td_disk_info_t *img)
{
	td_image_t *image;

	memset(img, 0, sizeof(*img));

	if (list_empty(&vbd->images))
		return -EINVAL;

	image        = tapdisk_vbd_first_image(vbd);
	*img         = image->info;

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
	return !(list_empty(&vbd->failed_requests) ||
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

int
tapdisk_vbd_pause(td_vbd_t *vbd)
{
	int err;

	DBG(TLOG_DBG, "pause requested\n");

	td_flag_set(vbd->state, TD_VBD_PAUSE_REQUESTED);

	err = tapdisk_vbd_quiesce_queue(vbd);
	if (err)
		return err;

	tapdisk_vbd_close_vdi(vbd);

	DBG(TLOG_DBG, "pause completed\n");

	td_flag_clear(vbd->state, TD_VBD_PAUSE_REQUESTED);
	td_flag_set(vbd->state, TD_VBD_PAUSED);

	return 0;
}

int
tapdisk_vbd_resume(td_vbd_t *vbd, int type, const char *path)
{
	int i, err;

	DBG(TLOG_DBG, "resume requested\n");

	if (!td_flag_test(vbd->state, TD_VBD_PAUSED)) {
		EPRINTF("resume request for unpaused vbd %s\n", vbd->name);
		return -EINVAL;
	}

	if (path) {
		free(vbd->name);
		vbd->name = strdup(path);
		if (!vbd->name) {
			EPRINTF("copying new vbd %s name failed\n", path);
			return -EINVAL;
		}
		vbd->type = type;
	}

	for (i = 0; i < TD_VBD_EIO_RETRIES; i++) {
		err = __tapdisk_vbd_open_vdi(vbd, TD_OPEN_STRICT);
		if (!err)
			break;

		sleep(TD_VBD_EIO_SLEEP);
	}

	if (err)
		return err;

	DBG(TLOG_DBG, "resume completed\n");

	tapdisk_vbd_start_queue(vbd);
	td_flag_clear(vbd->state, TD_VBD_PAUSED);
	td_flag_clear(vbd->state, TD_VBD_PAUSE_REQUESTED);
	tapdisk_vbd_check_state(vbd);

	DBG(TLOG_DBG, "state checked\n");

	return 0;
}

int
tapdisk_vbd_kick(td_vbd_t *vbd)
{
	int n;
	td_ring_t *ring;

	tapdisk_vbd_check_queue_state(vbd);

	ring = &vbd->ring;
	if (!ring->sring)
		return 0;

	n    = (ring->fe_ring.rsp_prod_pvt - ring->fe_ring.sring->rsp_prod);
	if (!n)
		return 0;

	vbd->kicks_out++;
	vbd->kicked += n;
	RING_PUSH_RESPONSES(&ring->fe_ring);
	ioctl(ring->fd, BLKTAP_IOCTL_KICK_FE, 0);

	DBG(TLOG_INFO, "kicking %d: rec: 0x%08llx, ret: 0x%08llx, kicked: "
	    "0x%08llx\n", n, vbd->received, vbd->returned, vbd->kicked);

	return n;
}

static inline void
tapdisk_vbd_write_response_to_ring(td_vbd_t *vbd, blkif_response_t *rsp)
{
	td_ring_t *ring;
	blkif_response_t *rspp;

	ring = &vbd->ring;
	rspp = RING_GET_RESPONSE(&ring->fe_ring, ring->fe_ring.rsp_prod_pvt);
	memcpy(rspp, rsp, sizeof(blkif_response_t));
	ring->fe_ring.rsp_prod_pvt++;
}

static void
tapdisk_vbd_callback(void *arg, blkif_response_t *rsp)
{
	td_vbd_t *vbd = (td_vbd_t *)arg;
	tapdisk_vbd_write_response_to_ring(vbd, rsp);
}

static void
tapdisk_vbd_make_response(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	blkif_request_t tmp;
	blkif_response_t *rsp;

	tmp = vreq->req;
	rsp = (blkif_response_t *)&vreq->req;

	rsp->id = tmp.id;
	rsp->operation = tmp.operation;
	rsp->status = vreq->status;

	DBG(TLOG_DBG, "writing req %d, sec 0x%08"PRIx64", res %d to ring\n",
	    (int)tmp.id, tmp.sector_number, vreq->status);

	if (rsp->status != BLKIF_RSP_OKAY)
		ERR(-vreq->error, "returning BLKIF_RSP %d", rsp->status);

	vbd->returned++;
	vbd->callback(vbd->argument, rsp);
}

static int
tapdisk_vbd_request_ttl(td_vbd_request_t *vreq,
			const struct timeval *now)
{
	struct timeval delta;
	timersub(now, &vreq->ts, &delta);
	return TD_VBD_REQUEST_TIMEOUT - delta.tv_sec;
}

static int
__tapdisk_vbd_request_timeout(td_vbd_request_t *vreq,
			      const struct timeval *now)
{
	int timeout;

	timeout = tapdisk_vbd_request_ttl(vreq, now) < 0;
	if (timeout)
		DBG(TLOG_INFO, "req %"PRIu64" timed out, retried %d times\n",
		    vreq->req.id, vreq->num_retries);

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

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->completed_requests) {
		tapdisk_vbd_make_response(vbd, vreq);
		list_del(&vreq->next);
		tapdisk_vbd_initialize_vreq(vreq);
	}
}

void
tapdisk_vbd_check_state(td_vbd_t *vbd)
{
	tapdisk_vbd_check_queue_state(vbd);

	if (td_flag_test(vbd->state, TD_VBD_QUIESCE_REQUESTED))
		tapdisk_vbd_quiesce_queue(vbd);

	if (td_flag_test(vbd->state, TD_VBD_PAUSE_REQUESTED))
		tapdisk_vbd_pause(vbd);

	if (td_flag_test(vbd->state, TD_VBD_SHUTDOWN_REQUESTED))
		tapdisk_vbd_close(vbd);
}

void
tapdisk_vbd_check_progress(td_vbd_t *vbd)
{
	time_t diff;
	struct timeval now, delta;

	if (list_empty(&vbd->pending_requests))
		return;

	gettimeofday(&now, NULL);
	timersub(&now, &vbd->ts, &delta);
	diff = delta.tv_sec;

	if (diff >= TD_VBD_WATCHDOG_TIMEOUT && tapdisk_vbd_queue_ready(vbd)) {
		DBG(TLOG_WARN, "%s: watchdog timeout: pending requests "
		    "idle for %ld seconds\n", vbd->name, diff);
		tapdisk_vbd_drop_log(vbd);
		return;
	}

	tapdisk_server_set_max_timeout(TD_VBD_WATCHDOG_TIMEOUT - diff);
}

/*
 * request submission 
 */

static int
tapdisk_vbd_check_queue(td_vbd_t *vbd)
{
	int err;
	td_image_t *image;

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
		if (vreq->status == BLKIF_RSP_ERROR &&
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
	td_image_t *image = treq.image, *prev, *tmp;
	int err;

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
		vreq->status = BLKIF_RSP_ERROR;
		vreq->error  = (vreq->error ? : err);
		if (err != -EBUSY) {
			vbd->errors++;
			ERR(err, "req %"PRIu64": %s 0x%04x secs to "
			    "0x%08"PRIx64, vreq->req.id,
			    (treq.op == TD_OP_WRITE ? "write" : "read"),
			    treq.secs, treq.sec);
		}
	}

	tapdisk_vbd_complete_vbd_request(vbd, vreq);
}

static void
__tapdisk_vbd_reissue_td_request(td_vbd_t *vbd,
				 td_image_t *image, td_request_t treq)
{
	td_image_t *parent;
	td_vbd_request_t *vreq;

	vreq = (td_vbd_request_t *)treq.private;
	gettimeofday(&vreq->last_try, NULL);

	vreq->submitting++;

	if (tapdisk_vbd_is_last_image(vbd, image)) {
		memset(treq.buf, 0, treq.secs << SECTOR_SHIFT);
		td_complete_request(treq, 0);
		goto done;
	}

	parent     = tapdisk_vbd_next_image(image);
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

		memset(clone.buf, 0, clone.secs << SECTOR_SHIFT);
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
	vreq  = treq.private;
	vbd   = vreq->vbd;

	tapdisk_vbd_mark_progress(vbd);

	if (tapdisk_vbd_queue_ready(vbd))
		__tapdisk_vbd_reissue_td_request(vbd, image, treq);
	else
		__tapdisk_vbd_complete_td_request(vbd, vreq, treq, -EBUSY);
}

static void
tapdisk_vbd_complete_td_request(td_request_t treq, int res)
{
	td_vbd_t *vbd;
	td_image_t *image, *leaf;
	td_vbd_request_t *vreq;

	image = treq.image;
	vreq  = treq.private;
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

	DBG(TLOG_DBG, "%s: req %d seg %d sec 0x%08llx "
	    "secs 0x%04x buf %p op %d res %d\n", image->name,
	    (int)treq.id, treq.sidx, treq.sec, treq.secs,
	    treq.buf, (int)vreq->req.operation, res);

	__tapdisk_vbd_complete_td_request(vbd, vreq, treq, res);
}

static inline void
queue_mirror_req(td_vbd_t *vbd, td_request_t clone)
{
	clone.image = vbd->secondary;
	td_queue_write(vbd->secondary, clone);
}

static inline void
tapdisk_vbd_submit_request(td_vbd_t *vbd, blkif_request_t *req,
		td_request_t treq)
{
	switch (req->operation)	{
	case BLKIF_OP_WRITE:
		treq.op = TD_OP_WRITE;
		/* it's important to queue the mirror request before queuing 
		 * the main one. If the main image runs into ENOSPC, the 
		 * mirroring could be disabled before td_queue_write returns, 
		 * so if the mirror request was queued after (which would then 
		 * not happen), we'd lose that write and cause the process to 
		 * hang with unacknowledged writes */
		if (vbd->secondary_mode == TD_VBD_SECONDARY_MIRROR)
			queue_mirror_req(vbd, treq);
		td_queue_write(treq.image, treq);
		break;

	case BLKIF_OP_READ:
		treq.op = TD_OP_READ;
		td_queue_read(treq.image, treq);
		break;
	}
}

static int
tapdisk_vbd_issue_request(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	char *page;
	td_ring_t *ring;
	td_image_t *image;
	td_request_t treq;
	uint64_t sector_nr;
	blkif_request_t *req;
	int i, err, id, nsects;
	struct timeval now;
	int treq_started = 0;

	req       = &vreq->req;
	id        = req->id;
	ring      = &vbd->ring;
	sector_nr = req->sector_number;
	image     = tapdisk_vbd_first_image(vbd);

	vreq->submitting = 1;

	tapdisk_vbd_mark_progress(vbd);
	vreq->last_try = vbd->ts;

	tapdisk_vbd_move_request(vreq, &vbd->pending_requests);

	err = tapdisk_vbd_check_queue(vbd);
	if (err) {
		vreq->error = err;
		goto fail;
	}

	err = tapdisk_image_check_ring_request(image, req);
	if (err) {
		vreq->error = err;
		goto fail;
	}

	memset(&treq, 0, sizeof(td_request_t));
	for (i = 0; i < req->nr_segments; i++) {
		nsects = req->seg[i].last_sect - req->seg[i].first_sect + 1;
		page   = (char *)MMAP_VADDR(ring->vstart, 
					   (unsigned long)req->id, i);
		page  += (req->seg[i].first_sect << SECTOR_SHIFT);

		if (treq_started) {
			if (page == treq.buf + (treq.secs << SECTOR_SHIFT)) {
				treq.secs += nsects;
			} else {
				tapdisk_vbd_submit_request(vbd, req, treq);
				treq_started = 0;
			}
		}

		if (!treq_started) {
			treq.id      = id;
			treq.sidx    = i;
			treq.buf     = page;
			treq.sec     = sector_nr;
			treq.secs    = nsects;
			treq.image   = image;
			treq.cb      = tapdisk_vbd_complete_td_request;
			treq.cb_data = NULL;
			treq.private = vreq;
			treq_started = 1;
		}

		DBG(TLOG_DBG, "%s: req %d seg %d sec 0x%08llx secs 0x%04x "
		    "buf %p op %d\n", image->name, id, i, treq.sec, treq.secs,
		    treq.buf, (int)req->operation);

		vreq->secs_pending += nsects;
		vbd->secs_pending  += nsects;
		if (vbd->secondary_mode == TD_VBD_SECONDARY_MIRROR &&
				req->operation == BLKIF_OP_WRITE) {
			vreq->secs_pending += nsects;
			vbd->secs_pending  += nsects;
		}

		if (i == req->nr_segments - 1) {
			tapdisk_vbd_submit_request(vbd, req, treq);
			treq_started = 0;
		}

		sector_nr += nsects;
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
	vreq->status = BLKIF_RSP_ERROR;
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
		vreq->error  = 0;
		vreq->status = BLKIF_RSP_OKAY;
		DBG(TLOG_DBG, "retry #%d of req %"PRIu64", "
		    "sec 0x%08"PRIx64", nr_segs: %d\n", vreq->num_retries,
		    vreq->req.id, vreq->req.sector_number,
		    vreq->req.nr_segments);

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
	blkif_request_t *req = &vreq->req;
	struct blkif_request_segment *seg;
	int write;

	write = req->operation == BLKIF_OP_WRITE;

	for (seg = &req->seg[0]; seg < &req->seg[req->nr_segments]; seg++) {
		int secs = seg->last_sect - seg->first_sect + 1;
		td_sector_count_add(&vbd->secs, secs, write);
	}
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

static int
tapdisk_vbd_kill_requests(td_vbd_t *vbd)
{
	td_vbd_request_t *vreq, *tmp;

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->new_requests) {
		vreq->status = BLKIF_RSP_ERROR;
		tapdisk_vbd_move_request(vreq, &vbd->completed_requests);
	}

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->failed_requests) {
		vreq->status = BLKIF_RSP_ERROR;
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

	err = tapdisk_vbd_reissue_failed_requests(vbd);
	if (err)
		return err;

	return tapdisk_vbd_issue_new_requests(vbd);
}

static void
tapdisk_vbd_pull_ring_requests(td_vbd_t *vbd)
{
	int idx;
	RING_IDX rp, rc;
	td_ring_t *ring;
	blkif_request_t *req;
	td_vbd_request_t *vreq;
	struct timeval now;

	ring = &vbd->ring;
	if (!ring->sring)
		return;

	gettimeofday(&now, NULL);

	rp   = ring->fe_ring.sring->req_prod;
	xen_rmb();

	for (rc = ring->fe_ring.req_cons; rc != rp; rc++) {
		req = RING_GET_REQUEST(&ring->fe_ring, rc);
		++ring->fe_ring.req_cons;

		idx  = req->id;
		vreq = &vbd->request_list[idx];

		ASSERT(list_empty(&vreq->next));
		ASSERT(vreq->secs_pending == 0);

		memcpy(&vreq->req, req, sizeof(blkif_request_t));
		vbd->received++;
		vreq->vbd = vbd;
		vreq->ts  = now;

		tapdisk_vbd_move_request(vreq, &vbd->new_requests);

		DBG(TLOG_DBG, "%s: request %d \n", vbd->name, idx);
	}
}

static int
tapdisk_vbd_pause_ring(td_vbd_t *vbd)
{
	int err;

	if (td_flag_test(vbd->state, TD_VBD_PAUSED))
		return 0;

	td_flag_set(vbd->state, TD_VBD_PAUSE_REQUESTED);

	err = tapdisk_vbd_quiesce_queue(vbd);
	if (err) {
		EPRINTF("%s: ring pause request on active queue\n", vbd->name);
		return err;
	}

	tapdisk_vbd_close_vdi(vbd);

	err = ioctl(vbd->ring.fd, BLKTAP2_IOCTL_PAUSE, 0);
	if (err)
		EPRINTF("%s: pause ioctl failed: %d\n", vbd->name, errno);
	else {
		td_flag_clear(vbd->state, TD_VBD_PAUSE_REQUESTED);
		td_flag_set(vbd->state, TD_VBD_PAUSED);
	}

	return err;
}

static int
tapdisk_vbd_resume_ring(td_vbd_t *vbd)
{
	int i, err, type;
	char message[BLKTAP2_MAX_MESSAGE_LEN];
	const char *path;

	memset(message, 0, sizeof(message));

	if (!td_flag_test(vbd->state, TD_VBD_PAUSED)) {
		EPRINTF("%s: resume message for unpaused vbd\n", vbd->name);
		return -EINVAL;
	}

	err = ioctl(vbd->ring.fd, BLKTAP2_IOCTL_REOPEN, &message);
	if (err) {
		EPRINTF("%s: resume ioctl failed: %d\n", vbd->name, errno);
		return err;
	}

	type = tapdisk_disktype_parse_params(message, &path);
	if (type < 0) {
		err = type;
		EPRINTF("%s: invalid resume string %s\n", vbd->name, message);
		goto out;
	}

	free(vbd->name);
	vbd->name = strdup(path);
	if (!vbd->name) {
		EPRINTF("resume malloc failed\n");
		err = -ENOMEM;
		goto out;
	}
	vbd->type = type;

	tapdisk_vbd_start_queue(vbd);
	err = __tapdisk_vbd_open_vdi(vbd, TD_OPEN_STRICT);
out:
	if (!err) {
		struct blktap2_params params;
		td_disk_info_t info;

		memset(&params, 0, sizeof(params));
		tapdisk_vbd_get_disk_info(vbd, &info);

		params.sector_size = info.sector_size;
		params.capacity    = info.size;
		snprintf(params.name, sizeof(params.name) - 1, "%s", message);

		ioctl(vbd->ring.fd, BLKTAP2_IOCTL_SET_PARAMS, &params);
		td_flag_clear(vbd->state, TD_VBD_PAUSED);
	}
	ioctl(vbd->ring.fd, BLKTAP2_IOCTL_RESUME, err);
	return err;
}

static int
tapdisk_vbd_check_ring_message(td_vbd_t *vbd)
{
	if (!vbd->ring.sring)
		return -EINVAL;

	switch (vbd->ring.sring->private.tapif_user.msg) {
	case 0:
		return 0;

	case BLKTAP2_RING_MESSAGE_PAUSE:
		return tapdisk_vbd_pause_ring(vbd);

	case BLKTAP2_RING_MESSAGE_RESUME:
		return tapdisk_vbd_resume_ring(vbd);

	case BLKTAP2_RING_MESSAGE_CLOSE:
		return tapdisk_vbd_close(vbd);

	default:
		return -EINVAL;
	}
}

static void
tapdisk_vbd_ring_event(event_id_t id, char mode, void *private)
{
	td_vbd_t *vbd;

	vbd = (td_vbd_t *)private;

	vbd->kicks_in++;
	tapdisk_vbd_pull_ring_requests(vbd);
	tapdisk_vbd_issue_requests(vbd);

	/* vbd may be destroyed after this call */
	tapdisk_vbd_check_ring_message(vbd);
}

void
tapdisk_vbd_stats(td_vbd_t *vbd, td_stats_t *st)
{
	td_image_t *image, *next;

	tapdisk_stats_enter(st, '{');
	tapdisk_stats_field(st, "name", "s", vbd->name);
	tapdisk_stats_field(st, "minor", "d", vbd->minor);

	tapdisk_stats_field(st, "secs", "[");
	tapdisk_stats_val(st, "llu", vbd->secs.rd);
	tapdisk_stats_val(st, "llu", vbd->secs.wr);
	tapdisk_stats_leave(st, ']');

	tapdisk_stats_field(st, "images", "[");
	tapdisk_vbd_for_each_image(vbd, image, next)
		tapdisk_image_stats(image, st);
	tapdisk_stats_leave(st, ']');

	tapdisk_stats_field(st,
			    "FIXME_enospc_redirect_count",
			    "llu", vbd->FIXME_enospc_redirect_count);

	tapdisk_stats_leave(st, '}');
}
