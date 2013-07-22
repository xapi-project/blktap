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
tap_ctl_close(const int id, const char *params, const int force,
	      struct timeval *timeout)
{
	int err;
	tapdisk_message_t message;

	if (!params)
		return -EINVAL;
	if (strnlen(params, TAPDISK_MESSAGE_STRING_LENGTH)
			>= TAPDISK_MESSAGE_STRING_LENGTH)
		return -ENAMETOOLONG;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_CLOSE;
	if (force)
		message.type = TAPDISK_MESSAGE_FORCE_SHUTDOWN;

	strcpy(message.u.string.text, params);

	err = tap_ctl_connect_send_and_receive(id, &message, timeout);
	if (err)
		return err;

	if (message.type == TAPDISK_MESSAGE_CLOSE_RSP) {
		err = message.u.response.error;
		if (err)
			EPRINTF("close failed: %s\n", strerror(err));
	} else {
		EPRINTF("got unexpected result '%s' from %d\n",
			tapdisk_message_name(message.type), id);
		err = -EINVAL;
	}

	return err;
}
