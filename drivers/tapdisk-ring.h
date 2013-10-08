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

#ifndef _TAPDISK_RING_H_
#define _TAPDISK_RING_H_

#include <inttypes.h>

#include <xenctrl.h>
#include <xen/io/ring.h>

typedef struct td_uring             td_uring_t;
typedef struct td_uring_header      td_uring_header_t;
typedef struct td_uring_request     td_uring_request_t;
typedef struct td_uring_response    td_uring_response_t;

struct td_uring {
	int                         ctlfd;

	char                       *shmem_path;
	char                       *ctlfd_path;

	void                       *shmem;
	void                       *ring_area;
	void                       *data_area;
};

struct td_uring_header {
	char                        cookie[8];
	uint32_t                    version;
	uint32_t                    shmem_size;
	uint32_t                    ring_size;
	uint32_t                    data_size;
	char                        reserved[4064];
};

struct td_uring_request {
	uint8_t                     op;
	uint64_t                    id;
	uint64_t                    sec;
	uint32_t                    secs;
	uint32_t                    offset;
};

struct td_uring_response {
	uint8_t                     op;
	uint64_t                    id;
	uint8_t                     status;
};

DEFINE_RING_TYPES(td_uring, td_uring_request_t, td_uring_response_t);

int tapdisk_uring_create(td_uring_t *, const char *location,
			uint32_t ring_size, uint32_t data_size);
int tapdisk_uring_destroy(td_uring_t *);

int tapdisk_uring_connect(td_uring_t *, const char *location);
int tapdisk_uring_disconnect(td_uring_t *);

int tapdisk_uring_poll(td_uring_t *);
int tapdisk_uring_kick(td_uring_t *);

#endif
