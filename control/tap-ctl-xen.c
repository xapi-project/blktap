/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * required by the Xen headers
 */
#include <inttypes.h>

#include <xen/xen.h>
#include <xen/grant_table.h>
#include <xen/event_channel.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tap-ctl.h"
#include "compiler.h"
#include "util.h"

int
tap_ctl_connect_xenblkif(const pid_t pid, const domid_t domid, const int devid, int poll_duration,
		int poll_idle_threshold,
		const grant_ref_t * grefs, const int order, const evtchn_port_t port,
		int proto, const char *pool, const int minor)
{
    tapdisk_message_t message;
    int i, err;

	memset(&message, 0, sizeof(message));
    message.type = TAPDISK_MESSAGE_XENBLKIF_CONNECT;
    message.cookie = minor;

    message.u.blkif.domid = domid;
    message.u.blkif.devid = devid;
    for (i = 0; i < 1 << order; i++)
        message.u.blkif.gref[i] = grefs[i];
    message.u.blkif.order = order;
    message.u.blkif.port = port;
    message.u.blkif.proto = proto;
    message.u.blkif.poll_duration = poll_duration;
    message.u.blkif.poll_idle_threshold = poll_idle_threshold;
    if (pool) {
        if (unlikely(strlen(pool) > (sizeof(message.u.blkif.pool) - 1))) {
            EPRINTF("pool name too long: %s\n", pool);
            return -ENAMETOOLONG;
        }
        safe_strncpy(message.u.blkif.pool, pool, sizeof(message.u.blkif.pool));
    } else {
        message.u.blkif.pool[0] = 0;
    }

    err = tap_ctl_connect_send_and_receive(pid, &message, NULL);
    if (err || message.type == TAPDISK_MESSAGE_ERROR) {
		if (!err)
			err = -message.u.response.error;
        if (err == -EALREADY)
            EPRINTF("failed to connect tapdisk[%d] to the ring: %s\n", pid,
                    strerror(-err));
	}
    return err;
}

int
tap_ctl_disconnect_xenblkif(const pid_t pid, const domid_t domid,
        const int devid, struct timeval *timeout)
{
    int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_XENBLKIF_DISCONNECT;
	message.u.blkif.domid = domid;
	message.u.blkif.devid = devid;

	err = tap_ctl_connect_send_and_receive(pid, &message, timeout);
	if (err)
		goto out;

	if (message.type == TAPDISK_MESSAGE_XENBLKIF_DISCONNECT_RSP
			|| message.type == TAPDISK_MESSAGE_ERROR)
		err = -message.u.response.error;
	else {
		EPRINTF("got unexpected result '%s' from tapdisk[%d]\n",
				tapdisk_message_name(message.type), pid);
		err = -EINVAL;
	}

out:
	if (err) {
		if (likely(err == -ENOENT))
			DPRINTF("failed to disconnect tapdisk[%d] from the ring: %s\n",
					pid, strerror(-err));
		else
			EPRINTF("failed to disconnect tapdisk[%d] from the ring: %s\n",
					pid, strerror(-err));
	}
	return err;
}
