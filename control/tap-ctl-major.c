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

#include "tap-ctl.h"

int
tap_ctl_blk_major(void)
{
	FILE *devices;
	int rv, major;

	devices = fopen("/proc/devices", "r");
	if (!devices) {
		rv = -errno;
		goto out;
	}

	do {
		char buf[32], *s;
		int n, offset;

		s = fgets(buf, sizeof(buf), devices);
		if (!s)
			break;

		major  = -ENODEV;
		offset = 0;

		n = sscanf(buf, "%d tapdev%n", &major, &offset);
		if (n == 1 && offset)
			break;
	} while (1);

	rv = major;

out:
	if (devices)
		fclose(devices);

	return rv;
}
