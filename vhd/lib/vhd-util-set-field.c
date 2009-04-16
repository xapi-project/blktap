/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

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
	off64_t eof;
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

	if (strcmp(field, "hidden")) {
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

	vhd.footer.hidden = (char)value;

	err = vhd_write_footer(&vhd, &vhd.footer);
	if (err == -ENOSPC && vhd_type_dynamic(&vhd) && value)
		/* if no space to write the primary footer, at least write the 
		 * backup footer so that it's possible to delete the VDI */
		err = vhd_write_footer_at(&vhd, &vhd.footer, 0);
		
 done:
	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> <-f field> <-v value> [-h help]\n");
	return -EINVAL;
}
