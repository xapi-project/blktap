/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 * Commonly used headers and definitions.
 */

// FIXME useless file?

#ifndef __BLKTAP_3_H__
#define __BLKTAP_3_H__

#include "compiler.h"

/* TODO fix in blktap2 headers */
#if 0
#define BLKTAP3_CONTROL_NAME        "blktap-control"
#define BLKTAP3_CONTROL_DIR         "/var/run/"BLKTAP3_CONTROL_NAME
#define BLKTAP3_CONTROL_SOCKET      "ctl"

#define BLKTAP3_ENOSPC_SIGNAL_FILE  "/var/run/tapdisk3-enospc"
#endif

#define TAPBACK_CTL_SOCK_PATH       "/var/run/tapback.sock"

#if 0
/**
 * Block I/O protocol
 *
 * Taken from linux/drivers/block/xen-blkback/common.h so that blkfront can
 * work both with blktap2 and blktap3.
 *
 * TODO linux/drivers/block/xen-blkback/common.h contains other definitions
 * necessary for allowing tapdisk3 to talk directly to blkfront. Find a way to
 * use the definitions from there.
 */
enum blkif_protocol {
       BLKIF_PROTOCOL_NATIVE = 1,
       BLKIF_PROTOCOL_X86_32 = 2,
       BLKIF_PROTOCOL_X86_64 = 3,
};
#endif
#endif /* __BLKTAP_3_H__ */
