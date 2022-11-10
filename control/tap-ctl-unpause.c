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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#include "tap-ctl.h"
#include "util.h"

int
tap_ctl_unpause(const int id, const int minor, const char *params, int flags,
		char *secondary, const char *logpath, const char *sockpath)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_RESUME;
	message.cookie = minor;
	message.u.params.flags = flags;

	if (params)
		safe_strncpy(message.u.params.path, params,
			     sizeof(message.u.params.path));
	if (secondary) {
		err = snprintf(message.u.params.secondary,
			       sizeof(message.u.params.secondary), "%s",
			       secondary);
		if (err >= sizeof(message.u.params.secondary)) {
			EPRINTF("secondary image name too long\n");
			return -ENAMETOOLONG;
		}
	}
	if (logpath || sockpath) {
		err = tap_ctl_connect_send_receive_ex(id, &message, logpath, sockpath, 0, NULL, NULL);
	}
	else {
		err = tap_ctl_connect_send_and_receive(id, &message, NULL);
	}

	if (err)
		return err;

	if (message.type == TAPDISK_MESSAGE_RESUME_RSP
			|| message.type == TAPDISK_MESSAGE_ERROR)
		err = -message.u.response.error;
	else {
		EPRINTF("got unexpected result '%s' from %d\n",
				tapdisk_message_name(message.type), id);
		err = -EINVAL;
	}

	if (err)
		EPRINTF("unpause failed: %s\n", strerror(-err));

	return err;
}
