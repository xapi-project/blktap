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

int
tap_ctl_open(const int id, const int minor, const char *params, int flags,
	     const int prt_minor, const char *secondary, int timeout,
	     const char* logpath, uint8_t key_size, uint8_t *encryption_key)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_OPEN;
	message.cookie = minor;
	message.u.params.devnum = minor;
	message.u.params.prt_devnum = prt_minor;
	message.u.params.req_timeout = timeout;
	message.u.params.flags = flags;

	err = snprintf(message.u.params.path,
		       sizeof(message.u.params.path) - 1, "%s", params);
	if (err >= sizeof(message.u.params.path)) {
		EPRINTF("name too long\n");
		return ENAMETOOLONG;
	}

	if (secondary) {
		err = snprintf(message.u.params.secondary,
			       sizeof(message.u.params.secondary) - 1, "%s",
			       secondary);
		if (err >= sizeof(message.u.params.secondary)) {
			EPRINTF("secondary image name too long\n");
			return ENAMETOOLONG;
		}
	}
	if (flags & (TAPDISK_MESSAGE_FLAG_ADD_LOG | TAPDISK_MESSAGE_FLAG_OPEN_ENCRYPTED)) {
		err = tap_ctl_connect_send_receive_ex(
			id, &message, logpath, NULL, key_size, encryption_key, NULL);
	}
	else {
		err = tap_ctl_connect_send_and_receive(id, &message, NULL);
	}

	if (encryption_key)
		free(encryption_key);

	if (err)
		return err;

	switch (message.type) {
	case TAPDISK_MESSAGE_OPEN_RSP:
		break;
	case TAPDISK_MESSAGE_ERROR:
		err = -message.u.response.error;
		EPRINTF("open failed: %s\n", strerror(-err));
		break;
	default:
		EPRINTF("got unexpected result '%s' from %d\n",
			tapdisk_message_name(message.type), id);
		err = EINVAL;
	}

	return err;
}
