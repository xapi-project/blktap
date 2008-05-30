/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"

int
vhd_util_create(int argc, char **argv)
{
	char *name;
	uint64_t size;
	int c, sparse, fixedsize, err;

	err       = -EINVAL;
	size      = 0;
	sparse    = 1;
	fixedsize = 0;
	name      = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:s:rbh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 's':
			err  = 0;
			size = strtoull(optarg, NULL, 10);
			break;
		case 'r':
			sparse = 0;
			break;
		case 'b':
			fixedsize = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (err || !name || optind != argc)
		goto usage;

	if (fixedsize)
		return vhd_create_fixed(name, size << 20,
				  (sparse ? HD_TYPE_DYNAMIC : HD_TYPE_FIXED));
	else
		return vhd_create(name, size << 20,
				  (sparse ? HD_TYPE_DYNAMIC : HD_TYPE_FIXED));

usage:
	printf("options: <-n name> <-s size (MB)> [-r reserve] "
			"[-b file_is_fixed_size] [-h help]\n");
	return -EINVAL;
}
