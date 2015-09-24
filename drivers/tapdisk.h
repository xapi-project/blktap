/* 
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
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
 * td_queue_tiocb().
 *
 * NOTE: tapdisk uses the number of sectors submitted per request as a 
 * ref count.  Plugins must use the callback function to communicate the
 * completion -- or error -- of every sector submitted to them.
 *
 * td_get_parent_id returns:
 *     0 if parent id successfully retrieved
 *     TD_NO_PARENT if no parent exists
 *     -errno on error
 */

#ifndef _TAPDISK_H_
#define _TAPDISK_H_

#include <time.h>
#include <stdint.h>

#include "list.h"
#include "compiler.h"
#include "tapdisk-log.h"
#include "tapdisk-utils.h"
#include "tapdisk-stats.h"

extern unsigned int PAGE_SIZE;
extern unsigned int PAGE_MASK;
extern unsigned int PAGE_SHIFT;

#define MAX_SEGMENTS_PER_REQ         11
#define MAX_REQUESTS                 32U
#define SECTOR_SHIFT                 9
#define DEFAULT_SECTOR_SIZE          512

#define TAPDISK_DATA_REQUESTS       (MAX_REQUESTS * MAX_SEGMENTS_PER_REQ)

//#define BLK_NOT_ALLOCATED            (-99)
#define TD_NO_PARENT                 1

#define MAX_RAMDISK_SIZE             1024000 /*500MB disk limit*/

#define TD_OP_READ                   0
#define TD_OP_WRITE                  1

#define TD_OPEN_QUIET                0x00001
#define TD_OPEN_QUERY                0x00002
#define TD_OPEN_RDONLY               0x00004
#define TD_OPEN_STRICT               0x00008
#define TD_OPEN_SHAREABLE            0x00010
#define TD_OPEN_ADD_CACHE            0x00020
#define TD_OPEN_VHD_INDEX            0x00040
#define TD_OPEN_LOG_DIRTY            0x00080
#define TD_OPEN_LOCAL_CACHE          0x00100
#define TD_OPEN_REUSE_PARENT         0x00200
#define TD_OPEN_SECONDARY            0x00400
#define TD_OPEN_STANDBY              0x00800
#define TD_IGNORE_ENOSPC             0x01000
#define TD_OPEN_NO_O_DIRECT          0x02000
#define TD_OPEN_THIN                 0x04000

#define TD_CREATE_SPARSE             0x00001
#define TD_CREATE_MULTITYPE          0x00002

#define td_flag_set(word, flag)      ((word) |= (flag))
#define td_flag_clear(word, flag)    ((word) &= ~(flag))
#define td_flag_test(word, flag)     ((word) & (flag))

typedef uint16_t                     td_uuid_t;
typedef uint32_t                     td_flag_t;
typedef uint64_t                     td_sector_t;
typedef struct td_disk_id            td_disk_id_t;
typedef struct td_disk_info          td_disk_info_t;
typedef struct td_request            td_request_t;
typedef struct td_driver_handle      td_driver_t;
typedef struct td_image_handle       td_image_t;
typedef struct td_sector_count       td_sector_count_t;
typedef struct td_vbd_request        td_vbd_request_t;
typedef struct td_vbd_handle         td_vbd_t;

/* 
 * Prototype of the callback to activate as requests complete.
 */
typedef void (*td_callback_t)(td_request_t, int);
typedef void (*td_vreq_callback_t)(td_vbd_request_t*, int, void*, int);

struct td_disk_id {
	char                        *name;
	int                          type;
	int                          flags;
};

struct td_disk_info {
	td_sector_t                  size;
        long                         sector_size;
	uint32_t                     info;
};

struct td_iovec {
	void                       *base;
	unsigned int                secs;
};

struct td_vbd_request {
	int                         op;
	td_sector_t                 sec;
	struct td_iovec            *iov;
	int                         iovcnt;

	td_vreq_callback_t          cb;
	void                       *token;
	const char                 *name;

	int                         error;
	int                         prev_error;

	int                         submitting;
	int                         secs_pending;
	int                         num_retries;
	struct timeval		    ts;
	struct timeval              last_try;

	td_vbd_t                   *vbd;
	struct list_head            next;
	struct list_head           *list_head;
};

struct td_request {
	int                          op;
	void                        *buf;

	td_sector_t                  sec;
	int                          secs;

	td_image_t                  *image;

	td_callback_t                cb;
	void                        *cb_data;

	int                          sidx;
	td_vbd_request_t            *vreq;
};

/* 
 * Structure describing the interface to a virtual disk implementation.
 * See note at the top of this file describing this interface.
 */
struct tap_disk {
	const char                  *disk_type;
	td_flag_t                    flags;
	int                          private_data_size;
	int (*td_open)               (td_driver_t *, const char *, td_flag_t);
	int (*td_close)              (td_driver_t *);
	int (*td_get_parent_id)      (td_driver_t *, td_disk_id_t *);
	int (*td_validate_parent)    (td_driver_t *, td_driver_t *, td_flag_t);
	void (*td_queue_read)        (td_driver_t *, td_request_t);
	void (*td_queue_write)       (td_driver_t *, td_request_t);
	void (*td_debug)             (td_driver_t *);
	void (*td_stats)             (td_driver_t *, td_stats_t *);

    /**
     * Callback to produce RRD output.
	 *
	 * Return a positive integer on success, 0 if the RRD has not been updated,
	 * and -errno on failure.
     */
    int (*td_rrd)                (td_driver_t *, char *buf,
									int * const off, int * const size);
};

struct td_sector_count {
	td_sector_t rd;
	td_sector_t wr;
};

static inline void
td_sector_count_add(td_sector_count_t *s, td_sector_t v, int write)
{
	if (write)
		s->wr += v;
	else
		s->rd += v;
}

void td_panic(void);

#endif
