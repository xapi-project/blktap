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
vhd_util_repair(int argc, char **argv)
{
	char *name;
	int err, c;
	off64_t eof;
	vhd_context_t vhd;

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

	err = vhd_open(&vhd, name, O_RDWR | O_DIRECT);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	err = vhd_end_of_data(&vhd, &eof);
	if (err) {
		printf("error finding end of data: %d\n", err);
		goto done;
	}

	err = vhd_write_footer_at(&vhd, &vhd.footer, eof);

 done:
	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> [-h help]\n");
	return -EINVAL;
}
