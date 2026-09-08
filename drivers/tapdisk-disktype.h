/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
/*#define DISK_TYPE_NTNX        16 - Deprecated */

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
