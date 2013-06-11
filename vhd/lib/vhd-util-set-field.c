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
vhd_util_set_field(int argc, char **argv)
{
	long value;
	int err, c;
	vhd_context_t vhd;
	char *name, *field;

	err   = -EINVAL;
	value = 0;
	name  = NULL;
	field = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:f:v:h")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'f':
			field = optarg;
			break;
		case 'v':
			err   = 0;
			value = strtol(optarg, NULL, 10);
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || !field || optind != argc || err)
		goto usage;

	if (strnlen(field, 25) >= 25) {
		printf("invalid field\n");
		goto usage;
	}

	if (strcmp(field, "hidden") && strcmp(field, "marker")) {
		printf("invalid field %s\n", field);
		goto usage;
	}

	if (value < 0 || value > 255) {
		printf("invalid value %ld\n", value);
		goto usage;
	}

	err = vhd_open(&vhd, name, VHD_OPEN_RDWR);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	if (!strcmp(field, "hidden")) {
		vhd.footer.hidden = (char)value;
		err = vhd_write_footer(&vhd, &vhd.footer);
		if (err == -ENOSPC && vhd_type_dynamic(&vhd) && value)
			/* if no space to write the primary footer, at least write the 
			 * backup footer so that it's possible to delete the VDI */
			err = vhd_write_footer_at(&vhd, &vhd.footer, 0);
	} else {
		err = vhd_set_marker(&vhd, (char)value);
	}
		
 	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> <-f field> <-v value> [-h help]\n");
	return -EINVAL;
}
