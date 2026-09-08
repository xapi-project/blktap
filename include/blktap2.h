/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

#define BLKTAP2_CONTROL_NAME           "blktap/control"
#define BLKTAP2_CONTROL_DIR            "/run/blktap-control"
#define BLKTAP2_NP_RUN_DIR             BLKTAP2_CONTROL_DIR"/tapdisk"
#define BLKTAP2_CONTROL_SOCKET         "ctl"
#define BLKTAP2_ENOSPC_SIGNAL_FILE     "/run/tapdisk-enospc"

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
