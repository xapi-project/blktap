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
tap_ctl_create(const char *params, int flags, const char *prt_path,
		char *secondary, int timeout, const char *uuid)
{
	int err, id;

	id = tap_ctl_spawn();
	if (id < 0)
		return id;

	err = tap_ctl_open(id, params, flags, prt_path, secondary,
			timeout, uuid);
	if (err)
		/* FIXME kill tapdisk */
		return err;

	return 0;
}
