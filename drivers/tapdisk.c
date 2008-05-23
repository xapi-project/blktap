/* 
 * Copyright (c) 2007, XenSource Inc.
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

#define TAPDISK

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "lock.h"
#include "tapdisk.h"
#include "blktaplib.h"
#include "tapdisk-message.h"

//#define TAPDISK_FILTER (TD_CHECK_INTEGRITY | TD_INJECT_FAULTS)

#if 1                                                                        
#define ASSERT(_p)                                                         \
do {                                                                       \
	if (!(_p)) {                                                       \
		DPRINTF("Assertion '%s' failed, line %d, file %s", #_p ,   \
			__LINE__, __FILE__);                               \
		*(int*)0 = 0;                                              \
	}                                                                  \
} while (0)
#else
#define ASSERT(_p) ((void)0)
#endif 

typedef struct fd_list_entry {
	int cookie;
	int  tap_fd;
	struct td_state *s;
	struct fd_list_entry **pprev, *next;
} fd_list_entry_t;

#define DBG(_level, _f, _a...) tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...) tlog_error(_err, _f, ##_a)

#define INPUT 0
#define OUTPUT 1
#define ONE_DAY (24 * 60 * 60)

static int maxfds;
static int fds[2];
static int run = 1;
static int connected_disks = 0;
static int force_lock_check = 0;
static fd_list_entry_t fd_start;

int do_cow_read(struct disk_driver *dd, blkif_request_t *req, 
		int sidx, uint64_t sector, int nr_secs);

#define td_for_each_disk(tds, drv) \
        for (drv = tds->disks; drv != NULL; drv = drv->next)

void usage(void) 
{
	fprintf(stderr, "blktap-utils: v1.0.0\n");
	fprintf(stderr, "usage: tapdisk <READ fifo> <WRITE fifo>\n");
        exit(-1);
}

void daemonize(void)
{
	int i;

	if (getppid()==1) return; /* already a daemon */
	if (fork() != 0) exit(0);

#if 0
	/*Set new program session ID and close all descriptors*/
	setsid();
	for (i = getdtablesize(); i >= 0; --i) close(i);

	/*Send all I/O to /dev/null */
	i = open("/dev/null",O_RDWR);
	dup(i); 
	dup(i);
#endif
	return;
}

void sig_handler(int sig)
{
	/*Received signal to close. If no disks are active, we close app.*/

	if (connected_disks < 1) run = 0;	
}

void inline debug_disks(struct td_state *s)
{
	struct disk_driver *dd;

	DBG(TLOG_WARN, "flags: 0x%08x, %lu reqs pending, "
	    "retries: %lu, received: %lu, returned: %lu, kicked: %lu\n",
	    s->flags, (s->received - s->kicked), s->retries,
	    s->received, s->returned, s->kicked);
	DBG(TLOG_WARN, "last queue activity at %010ld.%06ld\n",
	    s->ts.tv_sec, s->ts.tv_usec);

	tapdisk_debug_queue(&s->queue);
	td_for_each_disk(s, dd)
		if (dd->drv == dtypes[DISK_TYPE_VHD]->drv)
			vhd_debug(dd);
}

void debug(int sig)
{
	fd_list_entry_t *ptr;

	ptr = fd_start.next;
	while (ptr != NULL) {
		if (ptr->s)
			debug_disks(ptr->s);
		ptr = ptr->next;
	}

	tlog_flush();
}

static inline int LOCAL_FD_SET(fd_set *readfds)
{
	fd_list_entry_t *ptr;

	ptr = fd_start.next;
	while (ptr != NULL) {
		struct td_state *s = ptr->s;

		if (ptr->tap_fd) {
			if (!(s->flags & TD_DRAIN_QUEUE) &&
			    !(s->flags & TD_PAUSED)) {
				FD_SET(ptr->tap_fd, readfds);
				maxfds = (ptr->tap_fd > maxfds ? 
					  ptr->tap_fd : maxfds);
			}
			if (s->queue.poll_fd) {
				FD_SET(s->queue.poll_fd, readfds);
				maxfds = (s->queue.poll_fd > maxfds ?
					  s->queue.poll_fd : maxfds);
			}
		}
		ptr = ptr->next;
	}

	return 0;
}

static inline fd_list_entry_t *add_fd_entry(int tap_fd, struct td_state *s)
{
	fd_list_entry_t **pprev, *entry;
	int i;

	DPRINTF("Adding fd_list_entry\n");

	/*Add to linked list*/
	s->fd_entry   = entry = malloc(sizeof(fd_list_entry_t));
	entry->tap_fd = tap_fd;
	entry->s      = s;
	entry->next   = NULL;

	pprev = &fd_start.next;
	while (*pprev != NULL)
		pprev = &(*pprev)->next;

	*pprev = entry;
	entry->pprev = pprev;

	return entry;
}

static inline struct td_state *get_state(int cookie)
{
	fd_list_entry_t *ptr;

	ptr = fd_start.next;
	while (ptr != NULL) {
		if (ptr->cookie == cookie) return ptr->s;
		ptr = ptr->next;
	}
	return NULL;
}

static struct tap_disk *get_driver(int drivertype)
{
	/* blktapctrl has passed us the driver type */

	return dtypes[drivertype]->drv;
}

static inline void init_preq(pending_req_t *preq)
{
	memset(preq, 0, sizeof(pending_req_t));
}

static inline int
namedup(char **dup, char *name)
{
	*dup = NULL;

	if (strnlen(name, MAX_NAME_LEN) >= MAX_NAME_LEN)
		return -ENAMETOOLONG;
	
	*dup = strdup(name);
	if (*dup == NULL)
		return -ENOMEM;

	return 0;
}

static void free_state(struct td_state *s)
{
	if (!s)
		return;

	free(s->name);
	free(s->blkif);
	free(s->lock_uuid);
	free(s->ring_info);
	free(s);
}

static int state_init(struct td_state *s, char *name,
		      struct tap_disk *drv, int storage)
{
	int i, err;
	blkif_t *blkif = NULL;

	err = namedup(&s->name, name);
	if (err)
		goto fail;

	err = -ENOMEM;

	blkif = s->blkif = malloc(sizeof(blkif_t));
	if (!blkif)
		goto fail;

	s->ring_info = calloc(1, sizeof(tapdev_info_t));
	if (!s->ring_info)
		goto fail;

	for (i = 0; i < MAX_REQUESTS; i++)
		init_preq(&blkif->pending_list[i]);

	s->drv     = drv;
	s->storage = storage;

	return 0;

 fail:
	EPRINTF("failed to init tapdisk state for %s\n", name);
	return err;
}

static void free_driver(struct disk_driver *d)
{
	if (!d)
		return;

	free(d->name);
	free(d->private);
	free(d);
}

static struct disk_driver *disk_init(struct td_state *s, 
				     struct tap_disk *drv, 
				     char *name, td_flag_t flags, int storage)
{
	int err;
	struct disk_driver *dd;

	dd = calloc(1, sizeof(struct disk_driver));
	if (!dd)
		return NULL;
	
	dd->private = malloc(drv->private_data_size);
	if (!dd->private)
		goto fail;

	err = namedup(&dd->name, name);
	if (err)
		goto fail;

	dd->drv      = drv;
	dd->td_state = s;
	dd->flags    = flags;
	dd->storage  = storage;

	return dd;

fail:
	free_driver(dd);
	return NULL;
}

static int map_new_dev(struct td_state *s, int minor)
{
	char *devname;
	fd_list_entry_t *ptr;
	int err, tap_fd, page_size;
	tapdev_info_t *info = s->ring_info;

	err = asprintf(&devname,"%s/%s%d",
		       BLKTAP_DEV_DIR, BLKTAP_DEV_NAME, minor);
	if (err == -1)
		return -ENOMEM;
	
	tap_fd = open(devname, O_RDWR);
	if (tap_fd == -1) {
		err = -errno;
		EPRINTF("open failed on dev %s: %d", devname, errno);
		goto fail;
	} 
	info->fd = tap_fd;

	/*Map the shared memory*/
	page_size = getpagesize();
	info->mem = mmap(0, page_size * BLKTAP_MMAP_REGION_SIZE, 
			 PROT_READ | PROT_WRITE, MAP_SHARED, info->fd, 0);
	if ((long int)info->mem == -1) 
	{
		err = -errno;
		EPRINTF("mmap failed on dev %s!\n", devname);
		goto fail;
	}

	/* assign the rings to the mapped memory */ 
	info->sring = (blkif_sring_t *)((unsigned long)info->mem);
	BACK_RING_INIT(&info->fe_ring, info->sring, page_size);
	
	info->vstart = 
	        (unsigned long)info->mem + (BLKTAP_RING_PAGES * page_size);

	ioctl(info->fd, BLKTAP_IOCTL_SETMODE, BLKTAP_MODE_INTERPOSE);
	free(devname);

	/*Update the fd entry*/
	ptr = fd_start.next;
	while (ptr != NULL) {
		if (s == ptr->s) {
			ptr->tap_fd = tap_fd;
			break;
		}
		ptr = ptr->next;
	}	
	++connected_disks;

	return 0;

 fail:
	if (tap_fd != -1)
		close(tap_fd);
	free(devname);
	return -err;
}

static void close_disk(struct td_state *s)
{
	struct disk_driver *dd, *tmp;

	dd = s->disks;
	while (dd) {
		tmp = dd->next;
		dd->drv->td_close(dd);
		DPRINTF("closed %s\n", dd->name);
		free_driver(dd);
		dd = tmp;
	}
	s->disks = NULL;
}

static void unmap_disk(struct td_state *s)
{
	tapdev_info_t *info = s->ring_info;
	fd_list_entry_t *entry;

	int i;
	blkif_t *blkif = s->blkif;
	for (i = 0; blkif && i < MAX_REQUESTS; i++)
		if (blkif->pending_list[i].secs_pending)
			DPRINTF("%s: request %d has %d secs pending\n",
				__func__, i, blkif->pending_list[i].secs_pending);
	if (s->received != s->kicked)
		DPRINTF("%s: received %lu, kicked %lu\n",
			__func__, s->received, s->kicked);
	if (info && info->fe_ring.sring &&
	    (info->fe_ring.sring->req_prod != info->fe_ring.req_cons))
		DPRINTF("%s: req_prod: %u, req_cons: %u\n", __func__,
			info->fe_ring.sring->req_prod,
			info->fe_ring.req_cons);

	DPRINTF("%s: %ld retries\n", __func__, s->retries);

	close_disk(s);
	tapdisk_free_queue(&s->queue);

	entry = s->fd_entry;
	*entry->pprev = entry->next;
	if (entry->next)
		entry->next->pprev = entry->pprev;

	close(info->fd);
	--connected_disks;

	tapdisk_ipc_write(&s->ipc, TAPDISK_MESSAGE_CLOSE_RSP, 2);

	if (info != NULL && info->mem > 0)
	        munmap(info->mem, getpagesize() * BLKTAP_MMAP_REGION_SIZE);

	tlog_print_errors();

	return;
}

static int open_disk(struct td_state *s, struct tap_disk *drv,
		     char *path, td_flag_t flags)
{
	int err, iocbs;
	td_flag_t pflags;
	struct disk_id id;
	struct disk_driver *d;

	memset(&id, 0, sizeof(struct disk_id));
	s->disks = d = disk_init(s, drv, path, flags, s->storage);
	if (!d)
		return -ENOMEM;

	err = d->drv->td_open(d, path, flags);
	if (err) {
		free_driver(d);
		s->disks = NULL;
		return err;
	}
	DPRINTF("opened %s\n", path);
	pflags = flags | TD_OPEN_RDONLY;
	iocbs  = TAPDISK_DATA_REQUESTS + d->drv->private_iocbs;

	/* load backing files as necessary */
	while ((err = d->drv->td_get_parent_id(d, &id)) == 0) {
		struct disk_driver *new;
		
		if (id.drivertype > MAX_DISK_TYPES || 
		    !get_driver(id.drivertype) || !id.name) {
			err = -EINVAL;
			goto fail;
		}

		new = disk_init(s, get_driver(id.drivertype),
				id.name, pflags, s->storage);
		if (!new) {
			err = -ENOMEM;
			goto fail;
		}

		err = new->drv->td_open(new, new->name, pflags);
		if (err) {
			free_driver(new);
			goto fail;
		}

		err = d->drv->td_validate_parent(d, new, 0);
		if (err) {
			d->next = new;
			goto fail;
		}

		DPRINTF("opened %s\n", new->name);
		iocbs += new->drv->private_iocbs;
		d = d->next = new;
		free(id.name);
		memset(&id, 0, sizeof(struct disk_id));
	}

	s->info |= ((flags & TD_OPEN_RDONLY) ? VDISK_READONLY : 0);

	if (err >= 0) {
		struct tfilter *filter = NULL;
#ifdef TAPDISK_FILTER
		filter = tapdisk_init_tfilter(TAPDISK_FILTER, iocbs, s->size);
#endif

		if (s->flags & TD_PAUSED)
			err = 0;   /* queue already open */
		else
			err = tapdisk_init_queue(&s->queue, iocbs, 0, filter);

		if (!err)
			return 0;
	}

 fail:
	EPRINTF("failed opening disk: %d\n", err);
	if (id.name)
		free(id.name);
	d = s->disks;
	while (d) {
		struct disk_driver *tmp = d->next;
		d->drv->td_close(d);
		free_driver(d);
		d = tmp;
	}
	s->disks = NULL;
	return err;
}

#define EIO_SLEEP   1
#define EIO_RETRIES 10

static int reopen_disks(struct td_state *s)
{
	int i, err;
	struct disk_driver *dd, *next, *p = NULL;

	td_for_each_disk(s, dd) {
		dd->drv->td_close(dd);
		dd->flags |= TD_OPEN_STRICT;

		for (i = 0; i < EIO_RETRIES; i++) {
			err = dd->drv->td_open(dd, dd->name, dd->flags);
			if (err != -EIO)
				break;
			
			sleep(EIO_SLEEP);
		}
		if (err)
			goto fail;

		if (p) {
			for (i = 0; i < EIO_RETRIES; i++) {
				err = dd->drv->td_validate_parent(p, dd, 0);
				if (err != -EIO)
					break;

				sleep(EIO_SLEEP);
			}
			if (err) {
				dd->drv->td_close(dd);
				goto fail;
			}
		}

		p = dd;
	}

	return 0;

 fail:
	p = s->disks;
	while (p) {
		next = p->next;
		if (p != dd)
			p->drv->td_close(p);

		free_driver(p);
		p = next;
	}
	s->disks = NULL;

	return -1;
}

static void shutdown_disk(struct td_state *s)
{
	unmap_disk(s);
	free(s->fd_entry);
	free_state(s);
	sig_handler(SIGINT);
}

static inline int drain_queue(struct td_state *s)
{
	int i;
	blkif_t *blkif = s->blkif;

	for (i = 0; i < MAX_REQUESTS; i++)
		if (blkif->pending_list[i].secs_pending) {
			s->flags |= TD_DRAIN_QUEUE;
			return -EBUSY;
		}

	return 0;
}

static inline void start_queue(struct td_state *s)
{
	s->flags &= ~TD_DRAIN_QUEUE;
}

static inline void td_pause(struct td_state *s)
{
	if (s->flags & TD_PAUSED)
		return;

	if (drain_queue(s) != 0)
		s->flags |= TD_PAUSE;
	else {
		close_disk(s);
		tapdisk_ipc_write(&s->ipc, TAPDISK_MESSAGE_PAUSE_RSP, 2);
		s->flags |= TD_PAUSED;
	}
}

static inline void td_resume(struct td_state *s)
{
	int i, rsp, err = 0;

	if (s->flags & TD_PAUSED) {
		for (i = 0; i < EIO_RETRIES; i++) {
			err = open_disk(s, s->drv, s->name, s->flags);
			if (err != -EIO)
				break;

			sleep(EIO_SLEEP);
		}
	}

	if (!err) {
		s->flags &= ~(TD_PAUSE | TD_PAUSED);
		start_queue(s);
		rsp = TAPDISK_MESSAGE_RESUME_RSP;
	} else
		rsp = TAPDISK_MESSAGE_ERROR;

	tapdisk_ipc_write(&s->ipc, rsp, 2);
}

static inline void kill_queue(struct td_state *s)
{
	struct disk_driver *dd;

	s->flags |= TD_DEAD;
	td_for_each_disk(s, dd)
		dd->flags |= TD_OPEN_RDONLY;
}

static inline int queue_closed(struct td_state *s)
{
	return (s->flags & TD_CLOSED || s->flags & TD_DEAD);
}

static inline int check_locks(struct timeval *tv)
{
	int ret = (!tv->tv_sec || force_lock_check);
	force_lock_check = 0;
	return ret;
}

static int lock_disk(struct disk_driver *dd)
{
	int ret, lease, err;
	struct td_state *s = dd->td_state;

	err = lock(dd->name, s->lock_uuid, 0, s->lock_ro, &lease, &ret);

	if (!(s->flags & TD_LOCK_ENFORCE)) {
		if (!ret) {
			EPRINTF("TAPDISK LOCK ERROR: lock %s did not exist "
				"for %s: ret %d, err %d\n", s->lock_uuid,
				dd->name, ret, err);
			unlock(dd->name, s->lock_uuid, s->lock_ro, &ret);
		} else if (ret < 0)
			EPRINTF("TAPDISK LOCK ERROR: failed to renew lock %s "
				"for %s: ret %d, err %d\n", s->lock_uuid,
				dd->name, ret, err);
		return 10;
	}

	if (!ret) {
		EPRINTF("ERROR: VDI %s has been tampered with, "
			"closing queue! (err = %d)\n", dd->name, err);
		unlock(dd->name, s->lock_uuid, s->lock_ro, &ret);
		kill_queue(s);
		lease = ONE_DAY;
	} else if (ret < 0) {
		DBG(TLOG_WARN, "Failed to get lock for %s, err: %d\n",
		    dd->name, ret);
		s->flags |= TD_CLOSED;
		lease = 1;  /* retry in one second */
	} else {
		if (s->flags & TD_CLOSED)
			DBG(TLOG_WARN, "Reacquired lock for %s\n", dd->name);
		s->flags &= ~TD_CLOSED;
	}

	return lease;
}

static void assert_locks(struct timeval *tv)
{
	struct td_state *s;
	fd_list_entry_t *ptr;
	long lease, min_lease_time = ONE_DAY;

	if (!check_locks(tv))
		return;

	ptr = fd_start.next;
	while (ptr) {
		s = ptr->s;
		if (s->received &&
		    !(s->flags & TD_DEAD) && (s->flags & TD_LOCKING)) {
			lease = lock_disk(s->disks);
			min_lease_time = (lease < min_lease_time ?
					  lease : min_lease_time);
		}
		ptr = ptr->next;
	}

	tv->tv_sec  = min_lease_time;
	tv->tv_usec = 0;
}

static inline int write_rsp_to_ring(struct td_state *s, blkif_response_t *rsp)
{
	tapdev_info_t *info = s->ring_info;
	blkif_response_t *rsp_d;
	
	rsp_d = RING_GET_RESPONSE(&info->fe_ring, info->fe_ring.rsp_prod_pvt);
	memcpy(rsp_d, rsp, sizeof(blkif_response_t));
	info->fe_ring.rsp_prod_pvt++;
	
	return 0;
}

#define NFS_WINDOW 524288 /* (16 rpc slots * 32K NFS window) */
static inline int should_kick(struct td_state *s)
{
	tapdev_info_t *info = s->ring_info;

	if (info->fe_ring.rsp_prod_pvt == info->fe_ring.sring->rsp_prod)
		return 0;

	if (s->disks && s->disks->storage == TAPDISK_STORAGE_TYPE_NFS) {
		/* if we've already saturated NFS,
		   don't kick now to improve batching */
		if ((s->pending_data << 9) >= NFS_WINDOW)
			return 0;
	}

	return 1;
}

static inline void kick_responses(struct td_state *s)
{
	tapdev_info_t *info = s->ring_info;

	if (should_kick(s)) {
		int n      = (info->fe_ring.rsp_prod_pvt - 
			      info->fe_ring.sring->rsp_prod);
		s->kicked += n;
		DBG(TLOG_INFO, "kicking %d, rec: %lu, ret: %lu, kicked: %lu\n",
		    n, s->received, s->returned, s->kicked);

		RING_PUSH_RESPONSES(&info->fe_ring);
		ioctl(info->fd, BLKTAP_IOCTL_KICK_FE);
	}
}

static void make_response(struct td_state *s, pending_req_t *preq)
{
	blkif_request_t tmp;
	blkif_response_t *rsp;

	tmp = preq->req;
	rsp = (blkif_response_t *)&preq->req;

	rsp->id = tmp.id;
	rsp->operation = tmp.operation;
	rsp->status = preq->status;

	DBG(TLOG_DBG, "writing req %d, sec %" PRIu64 ", res %d to ring\n",
	    (int)tmp.id, tmp.sector_number, preq->status);

	if (rsp->status != BLKIF_RSP_OKAY)
		ERR(EIO, "returning BLKIF_RSP %d", rsp->status);

	write_rsp_to_ring(s, rsp);
	init_preq(preq);
	s->returned++;
}

static inline uint64_t
segment_start(blkif_request_t *req, int sidx)
{
	int i;
	uint64_t start = req->sector_number;

	for (i = 0; i < sidx; i++) 
		start += (req->seg[i].last_sect - req->seg[i].first_sect + 1);

	return start;
}

int send_responses(struct disk_driver *dd, int res, 
		   uint64_t sector, int nr_secs, int idx, void *private)
{
	pending_req_t   *preq;
	blkif_request_t *req;
	struct td_state *s = dd->td_state;
	blkif_t *blkif = s->blkif;
	int sidx = (int)(long)private, secs_done = nr_secs;

	if (idx > MAX_REQUESTS - 1) {
		ERR(-EINVAL, "invalid index returned(%u)!\n", idx);
		return -EINVAL;
	}
	preq = &blkif->pending_list[idx];
	req  = &preq->req;
	gettimeofday(&s->ts, NULL);

	DBG(TLOG_DBG, "req %d, sec %" PRIu64 " (%d secs) returned %d, "
	    "pending: %d\n", idx, req->sector_number, nr_secs, res,
	    preq->secs_pending);

	if (res == BLK_NOT_ALLOCATED) {
		if (queue_closed(s))
			res = -EIO;
		else {
			res = do_cow_read(dd, req, sidx, sector, nr_secs);
			if (res >= 0) {
				secs_done = res;
				res = 0;
			} else
				secs_done = 0;
		}
	}

	s->pending_data     -= secs_done;
	preq->secs_pending  -= secs_done;
	if (res) {
		preq->status = BLKIF_RSP_ERROR;
		if (res != -EBUSY)
			ERR(res, "req %d: %s %d secs to %" PRIu64, idx,
			    (req->operation == BLKIF_OP_WRITE ?
			     "write" : "read"), nr_secs, sector);
	}

	if (preq->status == BLKIF_RSP_ERROR &&
	    preq->num_retries < TD_MAX_RETRIES) {
		if (!preq->secs_pending && !(s->flags & TD_DEAD)) {
			gettimeofday(&preq->last_try, NULL);
			DBG(TLOG_INFO, "retry needed: %d, %" PRIu64 "\n",
			    idx, req->sector_number);
		}
		s->flags |= TD_RETRY_NEEDED;
		return res;
	}

	if (!preq->submitting && !preq->secs_pending)
		make_response(s, preq);

	return res;
}

int do_cow_read(struct disk_driver *dd, blkif_request_t *req, 
		int sidx, uint64_t sector, int nr_secs)
{
	int ret;
	char *page;
	blkif_t *blkif;
	pending_req_t *preq;
	uint64_t seg_start, seg_end;
	struct td_state  *s = dd->td_state;
	tapdev_info_t *info = s->ring_info;
	struct disk_driver *parent = dd->next;
	
	blkif     = s->blkif;
	preq      = &blkif->pending_list[req->id];
	seg_start = segment_start(req, sidx);
	seg_end   = seg_start + req->seg[sidx].last_sect + 1;
	
	ASSERT(sector >= seg_start && sector + nr_secs <= seg_end);

	page  = (char *)MMAP_VADDR(info->vstart, 
				   (unsigned long)req->id, sidx);
	page += (req->seg[sidx].first_sect << SECTOR_SHIFT);
	page += ((sector - seg_start) << SECTOR_SHIFT);

	if (!parent) {
		memset(page, 0, nr_secs << SECTOR_SHIFT);
		DBG(TLOG_DBG, "memset for %d, sec %" PRIu64 ", "
		    "nr_secs: %d\n", sidx, sector, nr_secs);
		return nr_secs;
	}

	/* reissue request to backing file */
	DBG(TLOG_DBG, "submitting %d, %" PRIu64 " (%d secs) to parent\n",
	    sidx, sector, nr_secs);

	preq->submitting++;
	ret = parent->drv->td_queue_read(parent, sector, nr_secs,
					 page, send_responses, 
					 req->id, (void *)(long)sidx);
	preq->submitting--;

	return ret;
}

static int queue_request(struct td_state *s, blkif_request_t *req)
{
	char *page;
	blkif_t *blkif;
	uint64_t sector_nr;
	tapdev_info_t *info;
	pending_req_t *preq;
	struct disk_driver *dd;
	int i, err, idx, ret, nsects, page_size;

	err       = 0;
	idx       = req->id;
	blkif     = s->blkif;
	sector_nr = req->sector_number;
	page_size = getpagesize();
	dd        = s->disks;
	preq      = &blkif->pending_list[idx];
	info      = s->ring_info;

	if (!dd) {
		preq->status = BLKIF_RSP_ERROR;
		make_response(s, preq);
		kick_responses(s);
		return 0;
	}
	
	preq->submitting = 1;
	gettimeofday(&s->ts, NULL);

	if (queue_closed(s)) {
		err = -EIO;
		goto send_response;
	}
	
	if ((dd->flags & TD_OPEN_RDONLY) &&
	    (req->operation == BLKIF_OP_WRITE)) {
		err = -EINVAL;
		goto send_response;
	}

	for (i = 0; i < req->nr_segments; i++) {
		nsects = req->seg[i].last_sect - req->seg[i].first_sect + 1;
		
		if ((req->seg[i].last_sect >= page_size >> 9) || (nsects <= 0)) {
			err = -EINVAL;
			goto send_response;
		}
		
		if (sector_nr >= s->size) {
			ERR(-EINVAL, "Sector request failed: "
			    "%s request, idx [%d,%d] size [%llu], "
			    "sector [%llu,%llu]\n",
			    (req->operation == BLKIF_OP_WRITE ? 
			     "WRITE" : "READ"), idx, i,
			    (long long unsigned)nsects << SECTOR_SHIFT,
			    (long long unsigned)sector_nr << SECTOR_SHIFT,
			    (long long unsigned)sector_nr);
			err = -EINVAL;
			goto send_response;
		}
		
		page  = (char *)MMAP_VADDR(info->vstart, 
					   (unsigned long)req->id, i);
		page += (req->seg[i].first_sect << SECTOR_SHIFT);
		preq->secs_pending += nsects;
		s->pending_data    += nsects;
		
		switch (req->operation)	{
		case BLKIF_OP_WRITE:
			ret = dd->drv->td_queue_write(dd, sector_nr, 
						      nsects, page, 
						      send_responses,
						      idx, (void *)(long)i);
			if (ret < 0) {
				preq->submitting--;
				return ret;
			}

			break;
		case BLKIF_OP_READ:
			ret = dd->drv->td_queue_read(dd, sector_nr,
						     nsects, page, 
						     send_responses,
						     idx, (void *)(long)i);
			if (ret < 0) {
				preq->submitting--;
				return ret;
			}

			break;
		default:
			EPRINTF("Unknown block operation\n");
			break;
		}
		sector_nr += nsects;
	}

 send_response:
	/* force write_rsp_to_ring for synchronous case */
	preq->submitting--;
	if (preq->secs_pending == 0)
		return send_responses(dd, err, 0, 0, idx, (void *)(long)0);

	return 0;
}

static inline void submit_requests(struct td_state *s)
{
	int ret;
	struct disk_driver *dd;

	DBG(TLOG_DBG, "flags: 0x%08x, queued: %d\n",
	    s->flags, tapdisk_queue_count(&s->queue));

	if (s->flags & TD_DEAD)
		tapdisk_cancel_all_tiocbs(&s->queue);
	else
		tapdisk_submit_all_tiocbs(&s->queue);
}

static void retry_requests(struct td_state *s)
{
	int i, cnt;
	blkif_t *blkif;
	pending_req_t *preq;
	struct timeval time;

	if (s->flags & TD_DRAIN_QUEUE ||
	    s->flags & TD_PAUSED)
		return;

	cnt   = 0;
	blkif = s->blkif;
	gettimeofday(&time, NULL);

	for (i = 0; i < MAX_REQUESTS; i++) {
		preq = &blkif->pending_list[i];

		if (preq->secs_pending)
			continue;

		if (preq->status != BLKIF_RSP_ERROR)
			continue;

		if (time.tv_sec - preq->last_try.tv_sec < TD_RETRY_INTERVAL) {
			cnt++;
			continue;
		}

		if (preq->num_retries >= TD_MAX_RETRIES && !preq->submitting) {
			DBG(TLOG_INFO, "req %"PRIu64" retried %d times\n",
			    preq->req.id, preq->num_retries);
			make_response(s, preq);
			continue;
		}

		s->retries++;
		preq->num_retries++;
		preq->status = BLKIF_RSP_OKAY;
		DBG(TLOG_DBG, "retry #%d of req %" PRIu64 ", "
		    "sec %" PRIu64 ", nr_segs: %d\n", preq->num_retries,
		    preq->req.id, preq->req.sector_number,
		    preq->req.nr_segments);

		if (queue_request(s, &preq->req))
			return;
	}
	
	if (!cnt)
		s->flags &= ~TD_RETRY_NEEDED;
}

static void issue_requests(struct td_state *s)
{
	int idx;
	blkif_t *blkif;
	RING_IDX rp, j;
	tapdev_info_t *info;
	blkif_request_t *req;

	blkif = s->blkif;
	info  = s->ring_info;

	DBG(TLOG_DBG, "req_prod: %u, req_cons: %u\n",
	    info->fe_ring.sring->req_prod,
	    info->fe_ring.req_cons);

	if (!run)
		return; /* We have received signal to close */

	if (!s->received) {
		if (s->flags & TD_LOCKING) {
			lock_disk(s->disks);
			force_lock_check = 1;
		}

		if (!queue_closed(s)) {
			if (reopen_disks(s)) {
				EPRINTF("reopening disks failed\n");
				kill_queue(s);
			} else 
				DPRINTF("reopening disks succeeded\n");
		}
	}

	rp = info->fe_ring.sring->req_prod; 
	rmb();
	for (j = info->fe_ring.req_cons; j != rp; j++) {
		req = RING_GET_REQUEST(&info->fe_ring, j);
		++info->fe_ring.req_cons;
		
		if (req == NULL)
			continue;
		
		idx = req->id;
		ASSERT(blkif->pending_list[idx].secs_pending == 0);
		memcpy(&blkif->pending_list[idx].req, req, sizeof(*req));
		blkif->pending_list[idx].status = BLKIF_RSP_OKAY;
		s->received++;

		DBG(TLOG_DBG, "queueing request %d, sec %" PRIu64 ", "
		    "nr_segs: %d\n", idx, req->sector_number,
		    req->nr_segments);

		queue_request(s, req);
	}
}

static inline void set_retry_timeout(struct timeval *tv)
{
	fd_list_entry_t *ptr;

	ptr = fd_start.next;
	while (ptr) {
		if (ptr->s->flags & TD_RETRY_NEEDED) {
			tv->tv_sec  = (tv->tv_sec < TD_RETRY_INTERVAL ?
				       tv->tv_sec : TD_RETRY_INTERVAL);
			tv->tv_usec = 0;
			return;
		}
		ptr = ptr->next;
	}
}

static inline int requests_pending(struct td_state *s)
{
	return (s->received - s->kicked);
}

static void check_progress(struct timeval *tv)
{
	struct td_state *s;
	struct timeval time;
	fd_list_entry_t *ptr;
	int TO = 10;

	ptr = fd_start.next;
	gettimeofday(&time, NULL);

	while (ptr) {
		s = ptr->s;
		if (!queue_closed(s) && requests_pending(s)) {
			if (time.tv_sec - s->ts.tv_sec > TO && !s->dumped_log) {
				DBG(TLOG_WARN, "time: %ld.%ld, ts: %ld.%ld\n", 
				    time.tv_sec, time.tv_usec,
				    s->ts.tv_sec, s->ts.tv_usec);
				debug(SIGUSR1);
				s->dumped_log = 1;
			} else if (!s->dumped_log)
				tv->tv_sec = (tv->tv_sec < TO ? tv->tv_sec : TO);
		}
		ptr = ptr->next;
	}
}

int
tapdisk_init(uint16_t cookie)
{
	struct td_state *s;
	fd_list_entry_t *entry;

	s = get_state(cookie);
	if (s) {
		EPRINTF("duplicate cookies! %u\n", cookie);
		return -EEXIST;
	}

	s = calloc(1, sizeof(struct td_state));
	if (!s) {
		EPRINTF("failed to allocate tapdisk state\n");
		return -ENOMEM;
	}

	s->ipc.rfd    = fds[READ];
	s->ipc.wfd    = fds[WRITE];
	s->ipc.cookie = cookie;

	entry         = add_fd_entry(0, s);
	entry->cookie = cookie;
	DPRINTF("Entered cookie %d\n", entry->cookie);

	return 0;
}

int
tapdisk_open(uint16_t cookie, char *path,
	     uint16_t drivertype, uint16_t storage, td_flag_t flags)
{
	int i, err;
	struct td_state *s;
	struct tap_disk *drv;

	s = get_state(cookie);
	if (!s)
		return -EINVAL;

	drv = get_driver(drivertype);
	if (!drv)
		return -EINVAL;
				
	DPRINTF("Loaded driver: name [%s], type [%d]\n",
		drv->disk_type, drivertype);

	err = state_init(s, path, drv, storage);
	if (err)
		return err;

	for (i = 0; i < EIO_RETRIES; i++) {
		err = open_disk(s, drv, path, flags);
		if (err != -EIO)
			break;

		sleep(EIO_SLEEP);
	}

	if (err)
		goto fail;

	return 0;

fail:
	if (s) {
		free(s->fd_entry);
		free_state(s);
	}
	return err;
}

int
tapdisk_new_device(uint16_t cookie, uint32_t devnum)
{
	struct td_state *s;

	s = get_state(cookie);
	DPRINTF("Retrieving state, cookie %d.....[%s]\n",
		cookie, (s == NULL ? "FAIL":"OK"));
	if (!s)
		return -EINVAL;

	return map_new_dev(s, devnum);
}

int tapdisk_get_image_info(uint16_t cookie, image_t *image)
{
	struct td_state *s;

	memset(image, 0, sizeof(image_t));

	s = get_state(cookie);
	if (!s)
		return -EINVAL;

	image->size    = s->size;
	image->secsize = s->sector_size;
	image->info    = s->info;

	return 0;
}

void
tapdisk_pause(uint16_t cookie)
{
	struct td_state *s;

	s = get_state(cookie);
	if (!s) {
		EPRINTF("got pause request for unknown cookie %u\n", cookie);
		return;
	}

	if (s->flags & TD_PAUSED) {
		tapdisk_ipc_write(&s->ipc, TAPDISK_MESSAGE_PAUSE_RSP, 2);
		return;
	}

	td_pause(s);
}

void
tapdisk_resume(uint16_t cookie)
{
	struct td_state *s;

	s = get_state(cookie);
	if (!s) {
		EPRINTF("got resume request for unknown cookie %u\n", cookie);
		return;
	}

	td_resume(s);
}

void
tapdisk_close(uint16_t cookie)
{
	struct td_state *s;

	s = get_state(cookie);
	if (!s) {
		EPRINTF("got close request for unknown cookie %u\n", cookie);
		return;
	}

	s->flags |= TD_SHUTDOWN_REQUESTED;
	if (drain_queue(s) == 0)
		shutdown_disk(s);
	else
		DPRINTF("%s: pending aio: draining queue\n", __func__);

	sig_handler(SIGINT);
}

int main(int argc, char *argv[])
{
	int len, msglen, ret;
	char *p, *buf;
	fd_set readfds, writefds;
	fd_list_entry_t *ptr, *next;
	struct td_state *s;
	char openlogbuf[128];
	struct timeval timeout = { .tv_sec = ONE_DAY, .tv_usec = 0 };
	td_ipc_t ipc;

	if (argc != 3) usage();

	daemonize();

	snprintf(openlogbuf, sizeof(openlogbuf), "TAPDISK[%d]", getpid());
	openlog(openlogbuf, LOG_CONS|LOG_ODELAY, LOG_DAEMON);
	open_tlog("/tmp/tapdisk.log", (64 << 10), TLOG_WARN, 0);

#if defined(CORE_DUMP)
#include <sys/resource.h>
	{
		/* set up core-dumps*/
		struct rlimit rlim;
		rlim.rlim_cur = RLIM_INFINITY;
		rlim.rlim_max = RLIM_INFINITY;
		if (setrlimit(RLIMIT_CORE, &rlim) < 0)
			EPRINTF("setrlimit failed: %d\n", errno);
	}
#endif

	/*Setup signal handlers*/
	signal (SIGBUS, sig_handler);
	signal (SIGINT, sig_handler);
	signal (SIGUSR1, debug);

	/*Open the control channel*/
	fds[READ]  = open(argv[1],O_RDWR|O_NONBLOCK);
	fds[WRITE] = open(argv[2],O_RDWR|O_NONBLOCK);

	if (fds[READ] < 0 || fds[WRITE] < 0) {
		EPRINTF("FD open failed [%d,%d]\n", fds[READ], fds[WRITE]);
		exit(-1);
	}

	ipc.rfd = fds[READ];
	ipc.wfd = fds[WRITE];

	while (run) {
		ret = 0;
		FD_ZERO(&readfds);
		FD_SET(fds[READ], &readfds);
		maxfds = fds[READ];

		/*Set all tap fds*/
		LOCAL_FD_SET(&readfds);

#if defined(USE_NFS_LOCKS)
		assert_locks(&timeout);
#else
		timeout.tv_sec = ONE_DAY;
#endif
		set_retry_timeout(&timeout);
		check_progress(&timeout);

		/*Wait for incoming messages*/
		DBG(TLOG_DBG, "selecting with timeout %ld.%ld\n", 
		    timeout.tv_sec, timeout.tv_usec);
		ret = select(maxfds + 1, &readfds, (fd_set *) 0, 
                             (fd_set *) 0, &timeout);
		DBG(TLOG_DBG, "select returned %d (%d)\n", ret, errno);

		if (ret < 0)
			continue;

		ptr = fd_start.next;
		while (ptr != NULL) {
			s    = ptr->s;
			next = ptr->next;

			if (!ptr->tap_fd)
				goto next;

			DBG(TLOG_INFO, "flags: 0x%08x, %d reqs pending, "
			    "received: %lu, returned: %lu, kicked: %lu\n",
			    s->flags, requests_pending(s),
			    s->received, s->returned, s->kicked);

			if (FD_ISSET(s->queue.poll_fd, &readfds))
				tapdisk_complete_tiocbs(&s->queue);

			retry_requests(s);
			if (FD_ISSET(ptr->tap_fd, &readfds))
				issue_requests(s);

			submit_requests(s);
			kick_responses(s);

			if (s->flags & TD_PAUSE)
				td_pause(s);

			if (s->flags & TD_SHUTDOWN_REQUESTED)
				if (drain_queue(s) == 0)
					shutdown_disk(s);

		next:
			ptr = next;
		}

		if (FD_ISSET(fds[READ], &readfds))
			tapdisk_ipc_read(&ipc, 0);
	}

	close(fds[READ]);
	close(fds[WRITE]);

	ptr = fd_start.next;
	while (ptr != NULL) {
		next = ptr->next;
		s = ptr->s;

		unmap_disk(s);
		free_state(s);
		free(ptr);
		ptr = next;
	}
	closelog();
	close_tlog();

	return 0;
}
