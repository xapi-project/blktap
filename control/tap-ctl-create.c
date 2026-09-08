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
#include <getopt.h>

#include "tap-ctl.h"
#include "blktap.h"

int
tap_ctl_create(const char *params, char **devname, int flags, int parent_minor,
		char *secondary, int timeout, const char *logpath)
{
	int err, id, minor;

	err = tap_ctl_allocate(&minor, devname);
	if (err)
		return err;

	id = tap_ctl_spawn();
	if (id < 0) {
		err = id;
		goto destroy;
	}

	err = tap_ctl_attach(id, minor);
	if (err)
		goto destroy;

	err = tap_ctl_open(id, minor, params, flags, parent_minor, secondary,
			   timeout, logpath, 0, NULL);
	if (err)
		goto detach;

	return 0;

detach:
	tap_ctl_detach(id, minor);
destroy:
	tap_ctl_free(minor);
	return err;
}
