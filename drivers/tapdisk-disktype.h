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

#ifndef __DISKTYPES_H__
#define __DISKTYPES_H__

#define DISK_TYPE_AIO         0
#define DISK_TYPE_SYNC        1
#define DISK_TYPE_VMDK        2
#define DISK_TYPE_VHDSYNC     3
#define DISK_TYPE_VHD         4
#define DISK_TYPE_RAM         5
#define DISK_TYPE_QCOW        6
#define DISK_TYPE_BLOCK_CACHE 7
#define DISK_TYPE_VINDEX      8
#define DISK_TYPE_LOG         9
#define DISK_TYPE_REMUS       10
#define DISK_TYPE_LCACHE      11
#define DISK_TYPE_LLECACHE    12
#define DISK_TYPE_LLPCACHE    13
#define DISK_TYPE_VALVE       14
#define DISK_TYPE_NBD         15

#define DISK_TYPE_NAME_MAX    32

typedef struct disk_info {
	const char     *name; /* driver name, e.g. 'aio' */
	char           *desc;  /* e.g. "raw image" */
	unsigned int    flags; 
} disk_info_t;

extern const disk_info_t     *tapdisk_disk_types[];
extern const struct tap_disk *tapdisk_disk_drivers[];

/* one single controller for all instances of disk type */
#define DISK_TYPE_SINGLE_CONTROLLER (1<<0)

/* filter driver without physical image data */
#define DISK_TYPE_FILTER            (1<<1)

int tapdisk_disktype_find(const char *name);
int tapdisk_disktype_parse_params(const char *params, const char **_path);

#endif
