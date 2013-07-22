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
tap_ctl_unpause(const int id, const char *params1, const char *params2,
        int flags, char *secondary)
{
	int err;
	tapdisk_message_t message;

    assert(params1);

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_RESUME;
	message.u.resume.flags = flags;

    if (strnlen(params1, TAPDISK_MESSAGE_MAX_PATH_LENGTH)
            >= TAPDISK_MESSAGE_MAX_PATH_LENGTH) {
        /* TODO log error */
        return ENAMETOOLONG;
    }

	strncpy(message.u.resume.params1, params1, TAPDISK_MESSAGE_MAX_PATH_LENGTH);

    if (params2) {
        if (strnlen(params2, TAPDISK_MESSAGE_MAX_PATH_LENGTH)
                >= TAPDISK_MESSAGE_MAX_PATH_LENGTH) {
            /* TODO log error */
            return ENAMETOOLONG;
        }
	    strncpy(message.u.resume.params2, params2,
                TAPDISK_MESSAGE_MAX_PATH_LENGTH);
    } else {
        message.u.resume.params2[0] = '\0';
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
