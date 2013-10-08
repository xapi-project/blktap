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
#include <unistd.h>
#include <string.h>

#include "tap-ctl.h"
#include "blktap2.h"

int
tap_ctl_check_blktap(const char **msg)
{
	FILE *f;
	int err = 0, minor;
	char name[32];

	memset(name, 0, sizeof(name));

	f = fopen("/proc/misc", "r");
	if (!f) {
		*msg = "failed to open /proc/misc";
		return -errno;
	}

	while (fscanf(f, "%d %32s", &minor, name) == 2) {
		if (!strcmp(name, BLKTAP2_CONTROL_NAME))
			goto out;
	}

	err = -ENOSYS;
	*msg = "blktap kernel module not installed";

out:
	fclose(f);
	return err;
}

int
tap_ctl_check(const char **msg)
{
	int err;

	err = tap_ctl_check_blktap(msg);
	if (err)
		goto out;

	err  = 0;
	*msg = "ok";

out:
	return err;
}
