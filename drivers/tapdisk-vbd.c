/* 
 * Copyright (c) 2008, XenSource Inc.
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
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "tapdisk-image.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-interface.h"

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
static int tapdisk_vbd_issue_requests(td_vbd_t *);
static void tapdisk_vbd_complete_vbd_request(td_vbd_t *, td_vbd_request_t *);

/* 
 * initialization
 */

static inline void
tapdisk_vbd_initialize_vreq(td_vbd_request_t *vreq)
{
	memset(vreq, 0, sizeof(td_vbd_request_t));
	INIT_LIST_HEAD(&vreq->next);
}

int
tapdisk_vbd_initialize(int rfd, int wfd, uint16_t uuid)
{
	int i;
	td_vbd_t *vbd;

	vbd = tapdisk_server_get_vbd(uuid);
	if (vbd) {
		EPRINTF("duplicate vbds! %u\n", uuid);
		return -EEXIST;
	}

	vbd = calloc(1, sizeof(td_vbd_t));
	if (!vbd) {
		EPRINTF("failed to allocate tapdisk state\n");
		return -ENOMEM;
	}

	vbd->uuid     = uuid;
	vbd->ipc.rfd  = rfd;
	vbd->ipc.wfd  = wfd;
	vbd->ipc.uuid = uuid;

	INIT_LIST_HEAD(&vbd->images);
	INIT_LIST_HEAD(&vbd->new_requests);
	INIT_LIST_HEAD(&vbd->pending_requests);
	INIT_LIST_HEAD(&vbd->failed_requests);
	INIT_LIST_HEAD(&vbd->completed_requests);
	INIT_LIST_HEAD(&vbd->next);
	gettimeofday(&vbd->ts, NULL);

	for (i = 0; i < MAX_REQUESTS; i++)
		tapdisk_vbd_initialize_vreq(vbd->request_list + i);

	tapdisk_server_add_vbd(vbd);

	return 0;
}

static int
tapdisk_vbd_validate_chain(td_vbd_t *vbd)
{
	int err;
	td_image_t *image, *parent, *tmp;

	DPRINTF("VBD CHAIN:\n");

	tapdisk_vbd_for_each_image(vbd, image, tmp) {
		DPRINTF("%s: %d\n", image->name, image->type);

		if (tapdisk_vbd_is_last_image(vbd, image))
			break;

		parent = tapdisk_vbd_next_image(image);
		err    = td_validate_parent(image, parent);
		if (err)
			return err;
	}

	return 0;
}

static void
tapdisk_vbd_close_vdi(td_vbd_t *vbd)
{
	td_image_t *image, *tmp;

	tapdisk_vbd_for_each_image(vbd, image, tmp) {
		td_close(image);
		tapdisk_image_free(image);
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
				       target->storage,
				       target->flags,
				       target->private);
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
						cache->flags,
						cache->storage);
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
	image = tapdisk_image_allocate(path, DISK_TYPE_VINDEX,
				       vbd->storage, flags, vbd);
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
					parent->storage,
					parent->flags,
					vbd);
	if (!log)
		return -ENOMEM;

	driver = tapdisk_driver_allocate(log->type,
					 log->name,
					 log->flags,
					 log->storage);
	if (!driver) {
		err = -ENOMEM;
		goto fail;
	}

	driver->info = parent->driver->info;
	log->driver  = driver;

	err = td_open(log);
	if (err)
		goto fail;

	list_add(&log->next, &vbd->images);
	return 0;

fail:
	tapdisk_image_free(log);
	return err;
}

/*
 * LVHD hack: 
 * raw volumes are named /dev/<sr-vg-name>-<sr-uuid>/LV-<sr-uuid>
 * vhd volumes are named /dev/<sr-vg-name>-<sr-uuid>/VHD-<sr-uuid>
 *
 * a live snapshot of a raw volume will result in the writeable volume's
 * name changing from the raw to vhd format, but this change will not be
 * reflected by xenstore.  hence this mess.
 */
static int
tapdisk_vbd_check_file(td_vbd_t *vbd)
{
	int i, err;
	regex_t re;
	size_t len, max;
	regmatch_t matches[4];
	char *new, *src, *dst, error[256];

	err = access(vbd->name, F_OK);
	if (!err)
		return 0;

#define HEX   "[A-Za-z0-9]"
#define UUID  HEX"\\{8\\}-"HEX"\\{4\\}-"HEX"\\{4\\}-"HEX"\\{4\\}-"HEX"\\{12\\}"
#define VG    "VG_"HEX"\\+"
#define TYPE  "\\(LV\\|VHD\\)"
#define RE    "\\(/dev/"VG"-"UUID"/\\)"TYPE"\\(-"UUID"\\)"

	err = regcomp(&re, RE, 0);
	if (err)
		goto regerr;

#undef HEX
#undef UUID
#undef VG
#undef TYPE
#undef RE

	err = regexec(&re, vbd->name, 4, matches, 0);
	if (err)
		goto regerr;

	max = strlen("VHD") + 1;
	for (i = 1; i < 4; i++) {
		if (matches[i].rm_so == -1 || matches[i].rm_eo == -1) {
			EPRINTF("%s: failed to tokenize name\n", vbd->name);
			err = -EINVAL;
			goto out;
		}

		max += matches[i].rm_eo - matches[i].rm_so;
	}

	new = malloc(max);
	if (!new) {
		EPRINTF("%s: failed to allocate new name\n", vbd->name);
		err = -ENOMEM;
		goto out;
	}

	src = new;
	for (i = 1; i < 4; i++) {
		dst = vbd->name + matches[i].rm_so;
		len = matches[i].rm_eo - matches[i].rm_so;

		if (i == 2) {
			if (memcmp(dst, "LV", len)) {
				EPRINTF("%s: bad name format\n", vbd->name);
				free(new);
				err = -EINVAL;
				goto out;
			}

			src += sprintf(src, "VHD");
			continue;
		}

		memcpy(src, dst, len + 1);
		src += len;
	}

	new[max - 1] = '\0';
	err = access(new, F_OK);
	if (err == -1) {
		EPRINTF("neither %s nor %s accessible\n",
			vbd->name, new);
		err = -errno;
		free(new);
		goto out;
	}

	DPRINTF("couldn't find %s, trying %s\n", vbd->name, new);

	err = 0;
	free(vbd->name);
	vbd->name = new;
	vbd->type = DISK_TYPE_VHD;

out:
	regfree(&re);
	return err;

regerr:
	regerror(err, &re, error, sizeof(error));
	EPRINTF("%s: regex failed: %s\n", vbd->name, error);
	err = -EINVAL;
	goto out;
}

static int
__tapdisk_vbd_open_vdi(td_vbd_t *vbd)
{
	char *file;
	int err, type;
	td_flag_t flags;
	td_disk_id_t id;
	td_image_t *image, *tmp;
	struct tfilter *filter = NULL;

	err = tapdisk_vbd_check_file(vbd);
	if (err)
		return err;

	flags = vbd->flags & ~TD_OPEN_SHAREABLE;
	file  = vbd->name;
	type  = vbd->type;

	for (;;) {
		err   = -ENOMEM;
		image = tapdisk_image_allocate(file, type,
					       vbd->storage, flags, vbd);

		if (file != vbd->name) {
			free(file);
			file = NULL;
		}

		if (!image)
			goto fail;

		err = td_load(image);
		if (err) {
			if (err != -ENODEV)
				goto fail;

			if (td_flag_test(flags, TD_OPEN_VHD_INDEX) &&
			    td_flag_test(flags, TD_OPEN_RDONLY)) {
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

		if (!image->storage)
			image->storage = vbd->storage;

		tapdisk_vbd_add_image(vbd, image);
		image = NULL;

		if (err == TD_NO_PARENT)
			break;

		file   = id.name;
		type   = id.drivertype;
		flags |= (TD_OPEN_RDONLY | TD_OPEN_SHAREABLE);
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

	err = tapdisk_vbd_validate_chain(vbd);
	if (err)
		goto fail;

	td_flag_clear(vbd->state, TD_VBD_CLOSED);

	return 0;

fail:
	if (image)
		tapdisk_image_free(image);

	tapdisk_vbd_close_vdi(vbd);

	return err;
}

static int
tapdisk_vbd_open_vdi(td_vbd_t *vbd, char *path,
		     uint16_t drivertype, uint16_t storage, td_flag_t flags)
{
	int i, err;
	struct tap_disk *ops;

	ops = tapdisk_server_find_driver_interface(drivertype);
	if (!ops)
		return -EINVAL;
	DPRINTF("Loaded %s driver for vbd %u %s 0x%08x\n",
		ops->disk_type, vbd->uuid, path, flags);

	err = tapdisk_namedup(&vbd->name, path);
	if (err)
		return err;

	vbd->flags   = flags;
	vbd->storage = storage;
	vbd->type    = drivertype;

	for (i = 0; i < TD_VBD_EIO_RETRIES; i++) {
		err = __tapdisk_vbd_open_vdi(vbd);
		if (err != -EIO)
			break;

		sleep(TD_VBD_EIO_SLEEP);
	}
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
	tapdisk_server_unregister_event(vbd->ring_event_id);
}

static int
tapdisk_vbd_map_device(td_vbd_t *vbd, uint32_t minor)
{
	
	char *devname;
	int err, psize;
	td_ring_t *ring;

	ring  = &vbd->ring;
	psize = getpagesize();

	err = asprintf(&devname, "%s/%s%d",
		       BLKTAP_DEV_DIR, BLKTAP_DEV_NAME, minor);
	if (err == -1)
		return -ENOMEM;

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

	ioctl(ring->fd, BLKTAP_IOCTL_SETMODE, BLKTAP_MODE_INTERPOSE);

	free(devname);
	return 0;

fail:
	if (ring->mem && ring->mem != MAP_FAILED)
		munmap(ring->mem, psize * BLKTAP_MMAP_REGION_SIZE);
	if (ring->fd != -1)
		close(ring->fd);
	ring->fd  = 0;
	ring->mem = NULL;
	free(devname);
	return err;
}

static int
tapdisk_vbd_unmap_device(td_vbd_t *vbd)
{
	int psize;

	psize = getpagesize();

	close(vbd->ring.fd);
	if (vbd->ring.mem > 0)
		munmap(vbd->ring.mem, psize * BLKTAP_MMAP_REGION_SIZE);

	return 0;
}

int
tapdisk_vbd_open(td_vbd_t *vbd, char *name, uint16_t type,
		 uint16_t storage, uint32_t minor, td_flag_t flags)
{
	int err;

	err = tapdisk_vbd_open_vdi(vbd, name, type, storage, flags);
	if (err)
		return err;

	err = tapdisk_vbd_map_device(vbd, minor);
	if (err)
		return err;

	err = tapdisk_vbd_register_event_watches(vbd);
	if (err)
		return err;

	return 0;
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

	tapdisk_vbd_queue_count(vbd, &new, &pending, &failed, &completed);

	DPRINTF("%s: state: 0x%08x, new: 0x%02x, pending: 0x%02x, "
		"failed: 0x%02x, completed: 0x%02x\n", 
		vbd->name, vbd->state, new, pending, failed, completed);
	DPRINTF("last activity: %010ld.%06ld, errors: 0x%04llx, "
		"retries: 0x%04llx, received: 0x%08llx, returned: 0x%08llx, "
		"kicked: 0x%08llx\n",vbd->ts.tv_sec, vbd->ts.tv_usec,
		vbd->errors, vbd->retries, vbd->received, vbd->returned,
		vbd->kicked);

	tapdisk_vbd_close_vdi(vbd);
	tapdisk_ipc_write(&vbd->ipc, TAPDISK_MESSAGE_CLOSE_RSP);
	tapdisk_vbd_unregister_events(vbd);
	tapdisk_vbd_unmap_device(vbd);
	tapdisk_server_remove_vbd(vbd);
	free(vbd->name);
	free(vbd);

	tlog_print_errors();

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
	    "returned: 0x%08llx, kicked: 0x%08llx\n",
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
	tlog_flush();
	td_flag_set(vbd->state, TD_VBD_LOG_DROPPED);
}

int
tapdisk_vbd_get_image_info(td_vbd_t *vbd, image_t *img)
{
	td_image_t *image;

	memset(img, 0, sizeof(image_t));

	if (list_empty(&vbd->images))
		return -EINVAL;

	image        = tapdisk_vbd_first_image(vbd);
	img->size    = image->info.size;
	img->secsize = image->info.sector_size;
	img->info    = image->info.info;

	return 0;
}

int
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
	return td_flag_test(vbd->state, TD_VBD_RETRY_NEEDED);
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

static int
tapdisk_vbd_close_and_reopen_image(td_vbd_t *vbd, td_image_t *image)
{
	int i, err;

	td_close(image);

	for (i = 0; i < TD_VBD_EIO_RETRIES; i++) {
		err = tapdisk_vbd_open_image(vbd, image);
		if (err != -EIO)
			break;

		sleep(TD_VBD_EIO_SLEEP);
	}

	if (err)
		td_flag_set(vbd->state, TD_VBD_CLOSED);

	return err;
}

int
tapdisk_vbd_pause(td_vbd_t *vbd)
{
	int err;

	td_flag_set(vbd->state, TD_VBD_PAUSE_REQUESTED);

	err = tapdisk_vbd_quiesce_queue(vbd);
	if (err)
		return err;

	tapdisk_vbd_close_vdi(vbd);

	td_flag_clear(vbd->state, TD_VBD_PAUSE_REQUESTED);
	td_flag_set(vbd->state, TD_VBD_PAUSED);
	tapdisk_ipc_write(&vbd->ipc, TAPDISK_MESSAGE_PAUSE_RSP);

	return 0;
}

int
tapdisk_vbd_resume(td_vbd_t *vbd, const char *path, uint16_t drivertype)
{
	int i, err;

	if (!td_flag_test(vbd->state, TD_VBD_PAUSED)) {
		EPRINTF("pause request for unpaused vbd %s\n", vbd->name);
		tapdisk_ipc_write(&vbd->ipc, TAPDISK_MESSAGE_ERROR);
		return -EINVAL;
	}

	free(vbd->name);
	vbd->name = strdup(path);
	if (!vbd->name) {
		EPRINTF("copying new vbd %s name failed\n", path);
		tapdisk_ipc_write(&vbd->ipc, TAPDISK_MESSAGE_ERROR);
		return -EINVAL;
	}
	vbd->type = drivertype;

	for (i = 0; i < TD_VBD_EIO_RETRIES; i++) {
		err = __tapdisk_vbd_open_vdi(vbd);
		if (err != -EIO)
			break;

		sleep(TD_VBD_EIO_SLEEP);
	}

	if (err) {
		tapdisk_ipc_write(&vbd->ipc, TAPDISK_MESSAGE_ERROR);
		return err;
	}

	tapdisk_vbd_start_queue(vbd);
	td_flag_clear(vbd->state, TD_VBD_PAUSED);
	td_flag_clear(vbd->state, TD_VBD_PAUSE_REQUESTED);
	tapdisk_ipc_write(&vbd->ipc, TAPDISK_MESSAGE_RESUME_RSP);

	return 0;
}

int
tapdisk_vbd_kick(td_vbd_t *vbd)
{
	int n;
	td_ring_t *ring;

	ring = &vbd->ring;
	if (!ring->sring)
		return 0;

	n    = (ring->fe_ring.rsp_prod_pvt - ring->fe_ring.sring->rsp_prod);
	if (!n)
		return 0;

	vbd->kicked += n;
	RING_PUSH_RESPONSES(&ring->fe_ring);
	ioctl(ring->fd, BLKTAP_IOCTL_KICK_FE);

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
		ERR(EIO, "returning BLKIF_RSP %d", rsp->status);

	tapdisk_vbd_write_response_to_ring(vbd, rsp);
	vbd->returned++;
}

void
tapdisk_vbd_check_state(td_vbd_t *vbd)
{
	td_vbd_request_t *vreq, *tmp;

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->failed_requests)
		if (vreq->num_retries >= TD_VBD_MAX_RETRIES)
			tapdisk_vbd_complete_vbd_request(vbd, vreq);

	if (!list_empty(&vbd->new_requests) ||
	    !list_empty(&vbd->failed_requests))
		tapdisk_vbd_issue_requests(vbd);

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->completed_requests) {
		tapdisk_vbd_make_response(vbd, vreq);
		list_del(&vreq->next);
		tapdisk_vbd_initialize_vreq(vreq);
	}

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
	int diff;
	struct timeval now;

	if (list_empty(&vbd->pending_requests))
		return;

	gettimeofday(&now, NULL);
	diff = now.tv_sec - vbd->ts.tv_sec;

	if (diff >= TD_VBD_WATCHDOG_TIMEOUT) {
		DBG(TLOG_WARN, "%s: watchdog timeout: pending requests "
		    "idle for %d seconds\n", vbd->name, diff);
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

	if (!vbd->reopened) {
		if (td_flag_test(vbd->state, TD_VBD_LOCKING)) {
			err = tapdisk_vbd_lock(vbd);
			if (err)
				return err;
		}

		image = tapdisk_vbd_first_image(vbd);
		td_flag_set(image->flags, TD_OPEN_STRICT);

		if (tapdisk_vbd_close_and_reopen_image(vbd, image))
			EPRINTF("reopening disks failed\n");
		else {
			DPRINTF("reopening disks succeeded\n");
			vbd->reopened = 1;
		}
	}

	return 0;
}

static void
tapdisk_vbd_complete_vbd_request(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	if (!vreq->submitting && !vreq->secs_pending) {
		if (vreq->status == BLKIF_RSP_ERROR &&
		    vreq->num_retries < TD_VBD_MAX_RETRIES &&
		    !td_flag_test(vbd->state, TD_VBD_DEAD) &&
		    !td_flag_test(vbd->state, TD_VBD_SHUTDOWN_REQUESTED))
			tapdisk_vbd_move_request(vreq, &vbd->failed_requests);
		else
			tapdisk_vbd_move_request(vreq, &vbd->completed_requests);
	}
}

static void
__tapdisk_vbd_complete_td_request(td_vbd_t *vbd, td_vbd_request_t *vreq,
				  td_request_t treq, int res)
{
	int err;

	err = (res <= 0 ? res : -res);
	vbd->secs_pending  -= treq.secs;
	vreq->secs_pending -= treq.secs;

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
	vbd   = (td_vbd_t *)image->private;
	vreq  = (td_vbd_request_t *)treq.private;

	gettimeofday(&vbd->ts, NULL);

	if (tapdisk_vbd_queue_ready(vbd))
		__tapdisk_vbd_reissue_td_request(vbd, image, treq);
	else
		__tapdisk_vbd_complete_td_request(vbd, vreq, treq, -EIO);
}

static void
tapdisk_vbd_complete_td_request(td_request_t treq, int res)
{
	td_vbd_t *vbd;
	td_image_t *image;
	td_vbd_request_t *vreq;

	image = treq.image;
	vbd   = (td_vbd_t *)image->private;
	vreq  = (td_vbd_request_t *)treq.private;

	gettimeofday(&vbd->ts, NULL);
	DBG(TLOG_DBG, "%s: req %d seg %d sec 0x%08llx "
	    "secs 0x%04x buf %p op %d res %d\n", image->name,
	    (int)treq.id, treq.sidx, treq.sec, treq.secs,
	    treq.buf, (int)vreq->req.operation, res);

	__tapdisk_vbd_complete_td_request(vbd, vreq, treq, res);
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

	req       = &vreq->req;
	id        = req->id;
	ring      = &vbd->ring;
	sector_nr = req->sector_number;
	image     = tapdisk_vbd_first_image(vbd);

	vreq->submitting = 1;
	gettimeofday(&vbd->ts, NULL);
	gettimeofday(&vreq->last_try, NULL);
	tapdisk_vbd_move_request(vreq, &vbd->pending_requests);

	err = tapdisk_vbd_check_queue(vbd);
	if (err)
		goto fail;

	err = tapdisk_image_check_ring_request(image, req);
	if (err)
		goto fail;

	for (i = 0; i < req->nr_segments; i++) {
		nsects = req->seg[i].last_sect - req->seg[i].first_sect + 1;
		page   = (char *)MMAP_VADDR(ring->vstart, 
					   (unsigned long)req->id, i);
		page  += (req->seg[i].first_sect << SECTOR_SHIFT);

		treq.id             = id;
		treq.sidx           = i;
		treq.buf            = page;
		treq.sec            = sector_nr;
		treq.secs           = nsects;
		treq.image          = image;
		treq.cb             = tapdisk_vbd_complete_td_request;
		treq.cb_data        = NULL;
		treq.private        = vreq;

		DBG(TLOG_DBG, "%s: req %d seg %d sec 0x%08llx secs 0x%04x "
		    "buf %p op %d\n", image->name, id, i, treq.sec, treq.secs,
		    treq.buf, (int)req->operation);

		vreq->secs_pending += nsects;
		vbd->secs_pending  += nsects;

		switch (req->operation)	{
		case BLKIF_OP_WRITE:
			treq.op = TD_OP_WRITE;
			td_queue_write(image, treq);
			break;

		case BLKIF_OP_READ:
			treq.op = TD_OP_READ;
			td_queue_read(image, treq);
			break;
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

		if (td_flag_test(vbd->state, TD_VBD_SHUTDOWN_REQUESTED))
			goto fail;

		if (vreq->error != -EBUSY &&
		    now.tv_sec - vreq->last_try.tv_sec < TD_VBD_RETRY_INTERVAL)
			continue;

		if (vreq->num_retries >= TD_VBD_MAX_RETRIES) {
		fail:
			DBG(TLOG_INFO, "req %"PRIu64"retried %d times\n",
			    vreq->req.id, vreq->num_retries);
			tapdisk_vbd_complete_vbd_request(vbd, vreq);
			continue;
		}

		vbd->retries++;
		vreq->num_retries++;
		vreq->error  = 0;
		vreq->status = BLKIF_RSP_OKAY;
		DBG(TLOG_DBG, "retry #%d of req %"PRIu64", "
		    "sec 0x%08"PRIx64", nr_segs: %d\n", vreq->num_retries,
		    vreq->req.id, vreq->req.sector_number,
		    vreq->req.nr_segments);

		err = tapdisk_vbd_issue_request(vbd, vreq);
		if (err)
			break;
	}

	if (list_empty(&vbd->failed_requests))
		td_flag_clear(vbd->state, TD_VBD_RETRY_NEEDED);
	else
		td_flag_set(vbd->state, TD_VBD_RETRY_NEEDED);

	return err;
}

static int
tapdisk_vbd_issue_new_requests(td_vbd_t *vbd)
{
	int err;
	td_vbd_request_t *vreq, *tmp;

	tapdisk_vbd_for_each_request(vreq, tmp, &vbd->new_requests) {
		err = tapdisk_vbd_issue_request(vbd, vreq);
		if (err)
			return err;
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

static int
tapdisk_vbd_issue_requests(td_vbd_t *vbd)
{
	int err;

	if (td_flag_test(vbd->state, TD_VBD_DEAD))
		return tapdisk_vbd_kill_requests(vbd);

	if (!tapdisk_vbd_queue_ready(vbd))
		return -EAGAIN;

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

	ring = &vbd->ring;
	if (!ring->sring)
		return;

	rp   = ring->fe_ring.sring->req_prod;
	rmb();

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

		tapdisk_vbd_move_request(vreq, &vbd->new_requests);

		DBG(TLOG_DBG, "%s: request %d \n", vbd->name, idx);
	}
}

static void
tapdisk_vbd_ring_event(event_id_t id, char mode, void *private)
{
	td_vbd_t *vbd;

	vbd = (td_vbd_t *)private;

	tapdisk_vbd_pull_ring_requests(vbd);
	tapdisk_vbd_issue_requests(vbd);
}
