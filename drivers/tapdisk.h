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
 * tracking.  Disks should transform these requests as necessary and return
 * the resulting iocbs to tapdisk using td_prep_[read,write]() and 
 * td_queue_tiocb().  tapdisk will submit iocbs in batches and signal
 * completions via td_complete().
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

#include "profile.h"
#include "blktaplib.h"
#include "tapdisk-queue.h"
#include "tapdisk-filter.h"
#include "disktypes.h"

/* Things disks need to know about, these should probably be in a higher-level
 * header. */
#define MAX_SEGMENTS_PER_REQ     11
#define SECTOR_SHIFT             9
#define DEFAULT_SECTOR_SIZE      512

#define TD_MAX_RETRIES           100
#define TD_RETRY_INTERVAL        1

#define MAX_IOFD                 2

#define BLK_NOT_ALLOCATED        (-99)
#define TD_NO_PARENT             1

#define TAPDISK_DATA_REQUESTS    (MAX_REQUESTS * MAX_SEGMENTS_PER_REQ)

#define MAX_RAMDISK_SIZE          1024000 /*500MB disk limit*/
#define MAX_NAME_LEN              1000

typedef uint32_t td_flag_t;

#define TD_OPEN_QUIET                0x00001
#define TD_OPEN_QUERY                0x00002
#define TD_OPEN_RDONLY               0x00004
#define TD_OPEN_STRICT               0x00008

#define TD_CREATE_SPARSE             0x00001
#define TD_CREATE_MULTITYPE          0x00002

#define TD_DRAIN_QUEUE               0x00001
#define TD_CHECKPOINT                0x00002
#define TD_LOCKING                   0x00004
#define TD_CLOSED                    0x00008
#define TD_DEAD                      0x00010
#define TD_RETRY_NEEDED              0x00020
#define TD_SHUTDOWN_REQUESTED        0x00040
#define TD_LOCK_ENFORCE              0x00080

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
	struct tlog *log;
};

/* This structure represents the state of an active virtual disk.           */
struct td_state {
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

	struct tqueue queue;
	struct disk_driver *disks;
};

/* Prototype of the callback to activate as requests complete.              */
typedef int (*td_callback_t)(struct disk_driver *dd, int res, uint64_t sector,
			     int nb_sectors, int id, void *private);

/* Structure describing the interface to a virtual disk implementation.     */
/* See note at the top of this file describing this interface.              */
struct tap_disk {
	const char *disk_type;
	int private_data_size;
	int private_iocbs;
	int (*td_open)           (struct disk_driver *dd, 
				  const char *name, td_flag_t flags);
	int (*td_close)          (struct disk_driver *dd);
	int (*td_queue_read)     (struct disk_driver *dd, uint64_t sector,
				  int nb_sectors, char *buf, td_callback_t cb,
				  int id, void *prv);
	int (*td_queue_write)    (struct disk_driver *dd, uint64_t sector,
				  int nb_sectors, char *buf, td_callback_t cb, 
				  int id, void *prv);
	int (*td_complete)       (struct disk_driver *dd,
				  struct tiocb *tiocb, int err);
	int (*td_get_parent_id)  (struct disk_driver *dd, struct disk_id *id);
	int (*td_validate_parent)(struct disk_driver *dd, 
				  struct disk_driver *p, td_flag_t flags);
	int (*td_snapshot)       (struct disk_id *parent_id, 
				  char *child_name, td_flag_t flags);
	int (*td_create)         (const char *name, 
				  uint64_t size, td_flag_t flags);
};

struct qcow_info {
        int       l1_size;
        int       l2_size;
        uint64_t *l1;
        uint64_t  secs;
	int       valid_td_fields;
	long      td_fields[TD_FIELD_INVALID];
};
int qcow_create(const char *filename, uint64_t total_size,
		const char *backing_file, int flags);
int qcow_set_field(struct disk_driver *dd, td_field_t field, long value);
int qcow_get_info(struct disk_driver *dd, struct qcow_info *info);
int qcow_coalesce(char *name);

void vhd_debug(struct disk_driver *dd);

#endif /*TAPDISK_H_*/
