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
#include <getopt.h>

#include "tap-ctl.h"
#include "blktap2.h"

int
tap_ctl_create(const char *params, char **devname, int flags, int parent_minor,
		char *secondary, int timeout)
{
	int err, id, minor;

	err = tap_ctl_allocate(&minor, devname);
	if (err)
		return err;

	id = tap_ctl_spawn(false);
	if (id < 0) {
		err = id;
		goto destroy;
	}

	err = tap_ctl_attach(id, minor);
	if (err)
		goto destroy;

	err = tap_ctl_open(id, minor, params, flags, parent_minor, secondary,
			timeout);
	if (err)
		goto detach;

	return 0;

detach:
	tap_ctl_detach(id, minor);
destroy:
	tap_ctl_free(minor);
	return err;
}
