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
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "tap-ctl.h"
#include "blktap2.h"

int
tap_ctl_free(const int minor)
{
	int fd, err;

	fd = open(BLKTAP2_CONTROL_DEVICE, O_RDONLY);
	if (fd == -1) {
		EPRINTF("failed to open control device: %d\n", errno);
		return errno;
	}

	err = ioctl(fd, BLKTAP2_IOCTL_FREE_TAP, minor);
	close(fd);

	return err ? -errno : 0;
}
