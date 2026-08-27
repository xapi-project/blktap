/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
vhd_util_repair(int argc, char **argv)
{
	char *name;
	int err, c;
	vhd_context_t vhd;
	int flags = VHD_OPEN_RDWR;

	name = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:bh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'b':
			flags |= VHD_OPEN_USE_BKP_FOOTER;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || optind != argc)
		goto usage;

	err = vhd_open(&vhd, name, flags);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	err = vhd_get_footer(&vhd);
	if (err) {
		printf("error reading footer %s: %d\n", name, err);
		return err;
	}
	if (uuid_is_null(vhd.footer.uuid))
		uuid_generate(vhd.footer.uuid);

	err = vhd_write_footer(&vhd, &vhd.footer);
	if (err)
		printf("error writing footer: %d\n", err);

	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> <-b use the back-up footer> [-h help]\n");
	return -EINVAL;
}
