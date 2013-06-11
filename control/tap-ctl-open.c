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
		const int prt_minor, const char *secondary, int timeout)
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

	err = tap_ctl_connect_send_and_receive(id, &message, NULL);
	if (err)
		return err;

	switch (message.type) {
	case TAPDISK_MESSAGE_OPEN_RSP:
		break;
	case TAPDISK_MESSAGE_ERROR:
		err = -message.u.response.error;
		EPRINTF("open failed, err %d\n", err);
		break;
	default:
		EPRINTF("got unexpected result '%s' from %d\n",
			tapdisk_message_name(message.type), id);
		err = EINVAL;
	}

	return err;
}
