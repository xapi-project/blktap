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
vhd_util_fill(int argc, char **argv)
{
	int err, c;
	char *buf, *name;
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

	err = vhd_open(&vhd, name, O_RDWR | O_DIRECT);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	err = vhd_get_bat(&vhd);
	if (err)
		goto done;

	err = posix_memalign((void **)&buf, 4096, vhd.header.block_size);
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
