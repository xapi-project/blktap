/*
 * Copyright (c) 2008, XenSource Inc.
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

#ifndef _TD_BLKTAP_H_
#define _TD_BLKTAP_H_

#include <stdint.h>
#include <stddef.h>
#include <linux/blktap.h>

#define BLKTAP_PAGE_SIZE sysconf(_SC_PAGE_SIZE)
#define wmb()

#define BLKTAP2_SYSFS_DIR              "/sys/class/blktap2"
#define BLKTAP2_CONTROL_NAME           "blktap-control"
#define BLKTAP2_CONTROL_DIR            "/var/run/"BLKTAP2_CONTROL_NAME
#define BLKTAP2_CONTROL_SOCKET         "ctl"
#define BLKTAP2_DIRECTORY              "/dev/xen/blktap-2"
#define BLKTAP2_CONTROL_DEVICE         BLKTAP2_DIRECTORY"/control"
#define BLKTAP2_RING_DEVICE            BLKTAP2_DIRECTORY"/blktap"
#define BLKTAP2_IO_DEVICE              BLKTAP2_DIRECTORY"/tapdev"
#define BLKTAP2_ENOSPC_SIGNAL_FILE     "/var/run/tapdisk-enospc"

/*
 * compat crap to reduce patch size for now.
 */

typedef blktap_sring_t     blkif_sring_t;
typedef blktap_back_ring_t blkif_back_ring_t;
typedef blktap_ring_req_t  blkif_request_t;
typedef blktap_ring_rsp_t  blkif_response_t;
#define blktap2_params blktap_device_info

#define blkif_request_segment blktap_segment
#define blktap_params blktap_device_info

#define BLKIF_OP_WRITE  BLKTAP_OP_WRITE
#define BLKIF_OP_READ   BLKTAP_OP_READ

#define BLKIF_RSP_ERROR BLKTAP_RSP_ERROR
#define BLKIF_RSP_OKAY  BLKTAP_RSP_OKAY

#define BLKIF_MAX_SEGMENTS_PER_REQUEST BLKTAP_SEGMENT_MAX
#define rmb()

#define BLKTAP_MMAP_PAGES       (BLKTAP_RING_SIZE * BLKTAP_SEGMENT_MAX)
#define BLKTAP_RING_PAGES       1
#define BLKTAP_MMAP_REGION_SIZE (BLKTAP_RING_PAGES + BLKTAP_MMAP_PAGES)
#define MMAP_VADDR(_vstart,_req,_seg)                   \
    ((_vstart) +                                        \
     ((_req) * BLKTAP_SEGMENT_MAX * BLKTAP_PAGE_SIZE) +    \
     ((_seg) * BLKTAP_PAGE_SIZE))

#define BLKTAP_IOCTL_KICK_FE           BLKTAP_IOCTL_RESPOND
#define BLKTAP2_IOCTL_ALLOC_TAP        BLKTAP_IOCTL_ALLOC_TAP
#define BLKTAP2_IOCTL_FREE_TAP         BLKTAP_IOCTL_FREE_TAP
#define BLKTAP2_IOCTL_CREATE_DEVICE    BLKTAP_IOCTL_CREATE_DEVICE
#define BLKTAP2_IOCTL_REMOVE_DEVICE    BLKTAP_IOCTL_REMOVE_DEVICE

#define BLKTAP2_IOCTL_SET_PARAMS       203
#define BLKTAP2_IOCTL_PAUSE            204
#define BLKTAP2_IOCTL_REOPEN           205
#define BLKTAP2_IOCTL_RESUME           206

#define BLKTAP2_MAX_MESSAGE_LEN        BLKTAP_NAME_MAX

#define BLKTAP2_RING_MESSAGE_PAUSE     1
#define BLKTAP2_RING_MESSAGE_RESUME    2
#define BLKTAP2_RING_MESSAGE_CLOSE     BLKTAP_RING_MESSAGE_CLOSE

#endif /* _TD_BLKTAP_H_ */
