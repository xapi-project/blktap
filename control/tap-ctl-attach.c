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
#include <string.h>
#include <getopt.h>

#include "tap-ctl.h"

int
tap_ctl_attach(const int id, const int minor)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_ATTACH;
	message.cookie = minor;

	err = tap_ctl_connect_send_and_receive(id, &message, NULL);
	if (err)
		return err;

	if (message.type == TAPDISK_MESSAGE_ATTACH_RSP) {
		err = message.u.response.error;
		if (err)
			EPRINTF("attach failed: %d\n", err);
	} else {
		EPRINTF("got unexpected result '%s' from %d\n",
			tapdisk_message_name(message.type), id);
		err = EINVAL;
	}

	return err;
}
