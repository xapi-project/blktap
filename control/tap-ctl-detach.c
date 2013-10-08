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
#include <string.h>
#include <getopt.h>

#include "tap-ctl.h"

int
tap_ctl_detach(const int id, const int minor)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_DETACH;
	message.cookie = minor;

	err = tap_ctl_connect_send_and_receive(id, &message, NULL);
	if (err)
		return err;

	if (message.type == TAPDISK_MESSAGE_DETACH_RSP) {
		err = message.u.response.error;
		if (err < 0)
			printf("detach failed: %d\n", err);
	} else {
		printf("got unexpected result '%s' from %d\n",
		       tapdisk_message_name(message.type), id);
		err = EINVAL;
	}

	return err;
}
