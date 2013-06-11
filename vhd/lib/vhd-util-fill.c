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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"

int
vhd_util_fill(int argc, char **argv)
{
	int err, c;
	char *name;
	void *buf;
	vhd_context_t vhd;
	uint64_t i, sec, secs;

	buf  = NULL;
	name = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:h")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || optind != argc)
		goto usage;

	err = vhd_open(&vhd, name, VHD_OPEN_RDWR);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	err = vhd_get_bat(&vhd);
	if (err)
		goto done;

	err = posix_memalign(&buf, 4096, vhd.header.block_size);
	if (err) {
		err = -err;
		goto done;
	}

	sec  = 0;
	secs = vhd.header.block_size >> VHD_SECTOR_SHIFT;

	for (i = 0; i < vhd.header.max_bat_size; i++) {
		err = vhd_io_read(&vhd, buf, sec, secs);
		if (err)
			goto done;

		err = vhd_io_write(&vhd, buf, sec, secs);
		if (err)
			goto done;

		sec += secs;
	}

	err = 0;

 done:
	free(buf);
	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> [-h help]\n");
	return -EINVAL;
}
