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
#include <assert.h>

#include "tap-ctl.h"

int
tap_ctl_unpause(const int id, int flags, char *secondary, const char *uuid,
		const char *new_params)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_RESUME;
	message.u.resume.flags = flags;

	if (!uuid || uuid[0] == '\0'
			|| strlen(uuid) >= TAPDISK_MAX_VBD_UUID_LENGTH) {
		EPRINTF("missing/invalid UUID\n");
		return EINVAL;
	}
	strcpy(message.u.resume.uuid, uuid);

    if (new_params) {
        if (strlen(new_params) >= TAPDISK_MESSAGE_MAX_PATH_LENGTH) {
            /* TODO log error */
            return ENAMETOOLONG;
        }
	    strncpy(message.u.resume.new_params, new_params,
                TAPDISK_MESSAGE_MAX_PATH_LENGTH);
    } else {
        message.u.resume.new_params[0] = '\0';
    }

	if (secondary) {
        if (strnlen(secondary, TAPDISK_MESSAGE_MAX_PATH_LENGTH)
                >= TAPDISK_MESSAGE_MAX_PATH_LENGTH) {
            /* TODO log error */
			return ENAMETOOLONG;
		}
	    strncpy(message.u.resume.secondary, secondary,
                TAPDISK_MESSAGE_MAX_PATH_LENGTH);
    } else {
        message.u.resume.secondary[0] = '\0';
	}

	err = tap_ctl_connect_send_and_receive(id, &message, NULL);
	if (err)
		return err;

	if (message.type == TAPDISK_MESSAGE_RESUME_RSP
            || message.type == TAPDISK_MESSAGE_ERROR)
		err = message.u.response.error;
	else {
		err = EINVAL;
		EPRINTF("got unexpected result '%s' from %d\n",
			tapdisk_message_name(message.type), id);
	}

	return err;
}
