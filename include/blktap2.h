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

#ifndef _BLKTAP_2_H_
#define _BLKTAP_2_H_

#define MISC_MAJOR_NUMBER              10

#define BLKTAP2_MAX_MESSAGE_LEN        256

#define BLKTAP2_RING_MESSAGE_PAUSE     1
#define BLKTAP2_RING_MESSAGE_RESUME    2
#define BLKTAP2_RING_MESSAGE_CLOSE     3

#define BLKTAP2_IOCTL_KICK_FE          1
#define BLKTAP2_IOCTL_ALLOC_TAP        200
#define BLKTAP2_IOCTL_FREE_TAP         201
#define BLKTAP2_IOCTL_CREATE_DEVICE    202
#define BLKTAP2_IOCTL_SET_PARAMS       203
#define BLKTAP2_IOCTL_PAUSE            204
#define BLKTAP2_IOCTL_REOPEN           205
#define BLKTAP2_IOCTL_RESUME           206
#define BLKTAP2_IOCTL_REMOVE_DEVICE    207

#define BLKTAP2_SYSFS_DIR              "/sys/class/blktap2"
#define BLKTAP2_CONTROL_NAME           "blktap-control"
#define BLKTAP2_CONTROL_DIR            "/var/run/"BLKTAP2_CONTROL_NAME
#define BLKTAP2_CONTROL_SOCKET         "ctl"
#define BLKTAP2_DIRECTORY              "/dev/xen/blktap-2"
#define BLKTAP2_CONTROL_DEVICE         BLKTAP2_DIRECTORY"/control"
#define BLKTAP2_RING_DEVICE            BLKTAP2_DIRECTORY"/blktap"
#define BLKTAP2_IO_DEVICE              BLKTAP2_DIRECTORY"/tapdev"
#define BLKTAP2_ENOSPC_SIGNAL_FILE     "/var/run/tapdisk-enospc"

struct blktap2_handle {
	unsigned int                   ring;
	unsigned int                   device;
	unsigned int                   minor;
};

struct blktap2_params {
	char                           name[BLKTAP2_MAX_MESSAGE_LEN];
	unsigned long long             capacity;
	unsigned long                  sector_size;
};

#endif
