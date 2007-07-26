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
 * 
 * Some notes on the tap_disk interface:
 * 
 * tap_disk aims to provide a generic interface to easily implement new 
 * types of image accessors.  The structure-of-function-calls is similar
 * to disk interfaces used in qemu/denali/etc, with the significant 
 * difference being the expectation of asynchronous rather than synchronous 
 * I/O.  The asynchronous interface is intended to allow lots of requests to
 * be pipelined through a disk, without the disk requiring any of its own
 * threads of control.  As such, a batch of requests is delivered to the disk
 * using:
 * 
 *    td_queue_[read,write]()
 * 
 * and passing in a completion callback, which the disk is responsible for 
 * tracking.  The end of a back is marked with a call to:
 * 
 *    td_submit()
 * 
 * The disk implementation must provide a file handle, which is used to 
 * indicate that it needs to do work.  tapdisk will add this file handle 
 * (returned from td_get_fd()) to it's poll set, and will call into the disk
 * using td_do_callbacks() whenever there is data pending.
 * 
 * Two disk implementations demonstrate how this interface may be used to 
 * implement disks with both asynchronous and synchronous calls.  block-aio.c
 * maps this interface down onto the linux libaio calls, while block-sync uses 
 * normal posix read/write.
 * 
 * A few things to realize about the sync case, which doesn't need to defer 
 * io completions:
 * 
 *   - td_queue_[read,write]() call read/write directly, and then call the 
 *     callback immediately.  The MUST then return a value greater than 0
 *     in order to tell tapdisk that requests have finished early, and to 
 *     force responses to be kicked to the clents.
 * 
 *   - The fd used for poll is an otherwise unused pipe, which allows poll to 
 *     be safely called without ever returning anything.
 *
 * NOTE: tapdisk uses the number of sectors submitted per request as a 
 * ref count.  Plugins must use the callback function to communicate the
 * completion--or error--of every sector submitted to them.
 *
 * td_get_parent_id returns:
 *     0 if parent id successfully retrieved
 *     TD_NO_PARENT if no parent exists
 *     -errno on error
 */

#ifndef TAPDISK_H_
#define TAPDISK_H_

#include <time.h>
#include <stdint.h>
#include "blktaplib.h"

/*If enabled, log all debug messages to syslog*/
#if 1
#include <syslog.h>
#define DPRINTF(_f, _a...) syslog(LOG_INFO, _f , ##_a)
#else
#define DPRINTF(_f, _a...) ((void)0)
#endif

/* Things disks need to know about, these should probably be in a higher-level
 * header. */
#define MAX_SEGMENTS_PER_REQ    11
#define SECTOR_SHIFT             9
#define DEFAULT_SECTOR_SIZE    512

#define TD_MAX_RETRIES         100
#define TD_RETRY_INTERVAL        1

#define MAX_IOFD                 2

#define BLK_NOT_ALLOCATED    (-99)
#define TD_NO_PARENT             1

#define MAX_RAMDISK_SIZE   1024000 /*500MB disk limit*/

typedef uint32_t td_flag_t;

#define TD_RDONLY          0x00001
#define TD_DRAIN_QUEUE     0x00002
#define TD_CHECKPOINT      0x00004
#define TD_MULTITYPE_CP    0x00008
#define TD_SPARSE          0x00010
#define TD_LOCKING         0x00020
#define TD_CLOSED          0x00040
#define TD_DEAD            0x00080
#define TD_RETRY_NEEDED    0x00100
#define TD_QUIET           0x00200
#define TD_STRICT          0x00400
#define TD_SHUTDOWN        0x00800
#define TD_LOCK_ENFORCE    0x01000

typedef enum {
	TD_FIELD_HIDDEN  = 0,
	TD_FIELD_INVALID = 1
} td_field_t;

struct vdi_field {
	char       *name;
	td_field_t  id;
};

static struct vdi_field td_vdi_fields[TD_FIELD_INVALID] = {
	{ .id = TD_FIELD_HIDDEN, .name = "hidden" }
};

struct td_state;
struct tap_disk;

struct disk_id {
	char *name;
	int drivertype;
};

struct disk_driver {
	int early;
	char *name;
	void *private;
	td_flag_t flags;
	int io_fd[MAX_IOFD];
	struct tap_disk *drv;
	struct td_state *td_state;
	struct disk_driver *next;
};

/* This structure represents the state of an active virtual disk.           */
struct td_state {
	struct disk_driver *disks;
	void *blkif;
	void *image;
	void *ring_info;
	void *fd_entry;
	char *cp_uuid;
	int   cp_drivertype;
	char *lock_uuid;
	int   lock_ro;
	td_flag_t flags;
	unsigned long      sector_size;
	unsigned long long size;
	unsigned int       info;
	unsigned long received, returned, kicked;
	struct timeval ts;
	int dumped_log;
};

/* Prototype of the callback to activate as requests complete.              */
typedef int (*td_callback_t)(struct disk_driver *dd, int res, uint64_t sector,
			     int nb_sectors, int id, void *private);

/* Structure describing the interface to a virtual disk implementation.     */
/* See note at the top of this file describing this interface.              */
struct tap_disk {
	const char *disk_type;
	int private_data_size;
	int (*td_open)           (struct disk_driver *dd, 
				  const char *name, td_flag_t flags);
	int (*td_queue_read)     (struct disk_driver *dd, uint64_t sector,
				  int nb_sectors, char *buf, td_callback_t cb,
				  int id, void *prv);
	int (*td_queue_write)    (struct disk_driver *dd, uint64_t sector,
				  int nb_sectors, char *buf, td_callback_t cb, 
				  int id, void *prv);
	int (*td_cancel_requests)(struct disk_driver *dd);
	int (*td_submit)         (struct disk_driver *dd);
	int (*td_close)          (struct disk_driver *dd);
	int (*td_do_callbacks)   (struct disk_driver *dd, int sid);
	int (*td_get_parent_id)  (struct disk_driver *dd, struct disk_id *id);
	int (*td_validate_parent)(struct disk_driver *dd, 
				  struct disk_driver *p, td_flag_t flags);
	int (*td_snapshot)       (struct disk_id *parent_id, 
				  char *child_name, td_flag_t flags);
	int (*td_create)         (const char *name, 
				  uint64_t size, td_flag_t flags);
};

typedef struct disk_info {
	int  idnum;
	char name[50];       /* e.g. "RAMDISK" */
	char handle[10];     /* xend handle, e.g. 'ram' */
	int  single_handler; /* is there a single controller for all */
	                     /* instances of disk type? */
#ifdef TAPDISK
	struct tap_disk *drv;	
#endif
} disk_info_t;

void debug_fe_ring(struct td_state *s);

extern struct tap_disk tapdisk_aio;
/* extern struct tap_disk tapdisk_sync;    */
/* extern struct tap_disk tapdisk_vmdk;    */
/* extern struct tap_disk tapdisk_vhdsync; */
extern struct tap_disk tapdisk_vhd;
extern struct tap_disk tapdisk_ram;
/* extern struct tap_disk tapdisk_qcow;    */

#define MAX_DISK_TYPES     20

#define DISK_TYPE_AIO      0
#define DISK_TYPE_SYNC     1
#define DISK_TYPE_VMDK     2
#define DISK_TYPE_VHDSYNC  3
#define DISK_TYPE_VHD      4
#define DISK_TYPE_RAM      5
#define DISK_TYPE_QCOW     6


/*Define Individual Disk Parameters here */
static disk_info_t null_disk = {
	-1,
	"null disk",
	"null",
	0,
#ifdef TAPDISK
	0,
#endif
};

static disk_info_t aio_disk = {
	DISK_TYPE_AIO,
	"raw image (aio)",
	"aio",
	0,
#ifdef TAPDISK
	&tapdisk_aio,
#endif
};
/*
static disk_info_t sync_disk = {
	DISK_TYPE_SYNC,
	"raw image (sync)",
	"sync",
	0,
#ifdef TAPDISK
	&tapdisk_sync,
#endif
};

static disk_info_t vmdk_disk = {
	DISK_TYPE_VMDK,
	"vmware image (vmdk)",
	"vmdk",
	1,
#ifdef TAPDISK
	&tapdisk_vmdk,
#endif
};

static disk_info_t vhdsync_disk = {
	DISK_TYPE_VHDSYNC,
	"virtual server image (vhd) - synchronous",
	"vhdsync",
	1,
#ifdef TAPDISK
	&tapdisk_vhdsync,
#endif
};
*/
static disk_info_t vhd_disk = {
	DISK_TYPE_VHD,
	"virtual server image (vhd)",
	"vhd",
	0,
#ifdef TAPDISK
	&tapdisk_vhd,
#endif
};

static disk_info_t ram_disk = {
	DISK_TYPE_RAM,
	"ramdisk image (ram)",
	"ram",
	1,
#ifdef TAPDISK
	&tapdisk_ram,
#endif
};
/*
static disk_info_t qcow_disk = {
	DISK_TYPE_QCOW,
	"qcow disk (qcow)",
	"qcow",
	0,
#ifdef TAPDISK
	&tapdisk_qcow,
#endif
};
*/
/*Main disk info array */
static disk_info_t *dtypes[] = {
	&aio_disk,
	&null_disk, /* &sync_disk, */
	&null_disk, /* &vmdk_disk, */
        &null_disk, /* &vhdsync_disk, */
	&vhd_disk,
	&ram_disk,
	&null_disk, /* &qcow_disk, */
};

typedef struct driver_list_entry {
	struct blkif *blkif;
	struct driver_list_entry **pprev, *next;
} driver_list_entry_t;

typedef struct fd_list_entry {
	int cookie;
	int  tap_fd;
	struct td_state *s;
	struct fd_list_entry **pprev, *next;
} fd_list_entry_t;

int qcow_create(const char *filename, uint64_t total_size,
		const char *backing_file, int flags);
int vhd_create(const char *filename, uint64_t total_size,
		const char *backing_file, int flags);

struct qcow_info {
        int       l1_size;
        int       l2_size;
        uint64_t *l1;
        uint64_t  secs;
	int       valid_td_fields;
	long      td_fields[TD_FIELD_INVALID];
};
int qcow_set_field(struct disk_driver *dd, td_field_t field, long value);
int qcow_get_info(struct disk_driver *dd, struct qcow_info *info);
int qcow_coalesce(char *name);

struct vhd_info {
        int       spb;
        int       bat_entries;
        uint32_t *bat;
        uint64_t  secs;
	long      td_fields[TD_FIELD_INVALID];
};
int vhd_set_field(struct disk_driver *dd, td_field_t field, long value);
int vhd_get_info(struct disk_driver *dd, struct vhd_info *info);
int vhd_coalesce(char *name);
void vhd_debug(struct disk_driver *dd);
int vhd_repair(struct disk_driver *dd);

#endif /*TAPDISK_H_*/
