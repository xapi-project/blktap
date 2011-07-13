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

#define BLKTAP2_SYSFS_DIR              "/sys/class/blktap2"
#define BLKTAP2_CONTROL_NAME           "blktap-control"
#define BLKTAP2_CONTROL_DIR            "/var/run/"BLKTAP2_CONTROL_NAME
#define BLKTAP2_CONTROL_SOCKET         "ctl"
#define BLKTAP2_DIRECTORY              "/dev/xen/blktap-2"
#define BLKTAP2_CONTROL_DEVICE         BLKTAP2_DIRECTORY"/control"
#define BLKTAP2_RING_DEVICE            BLKTAP2_DIRECTORY"/blktap"
#define BLKTAP2_IO_DEVICE              BLKTAP2_DIRECTORY"/tapdev"
#define BLKTAP2_ENOSPC_SIGNAL_FILE     "/var/run/tapdisk-enospc"

#endif /* _TD_BLKTAP_H_ */
