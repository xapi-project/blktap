/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
		char *secondary, const char *logpath)
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
	if (logpath) {
		err = tap_ctl_connect_send_receive_ex(id, &message, logpath, 0, NULL, NULL);
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
