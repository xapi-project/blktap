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

int
tap_ctl_close(const int id, const int minor, const int force,
            struct timeval *timeout)
{
    int err;
    tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_CLOSE;
	if (force)
		message.type = TAPDISK_MESSAGE_FORCE_SHUTDOWN;
	message.cookie = minor;

	err = tap_ctl_connect_send_and_receive(id, &message, timeout);
	if (err)
		return err;

	if (message.type == TAPDISK_MESSAGE_CLOSE_RSP
			|| message.type == TAPDISK_MESSAGE_ERROR)
		err = -message.u.response.error;
	else {
		EPRINTF("got unexpected result '%s' from %d\n",
				tapdisk_message_name(message.type), id);
		return err;
	}

    if (err)
        EPRINTF("close failed: %s\n", strerror(-err));

    return err;
}
