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
vhd_util_query(int argc, char **argv)
{
	char *name;
	vhd_context_t vhd;
	off64_t currsize;
	int ret, err, c, size, physize, parent, fields, depth;

	name    = NULL;
	size    = 0;
	physize = 0;
	parent  = 0;
	fields  = 0;
	depth   = 0;

	if (!argc || !argv) {
		err = -EINVAL;
		goto usage;
	}

	optind = 0;
	while ((c = getopt(argc, argv, "n:vspfdh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'v':
			size = 1;
			break;
		case 's':
			physize = 1;
			break;
		case 'p':
			parent = 1;
			break;
		case 'f':
			fields = 1;
			break;
		case 'd':
			depth = 1;
			break;
		case 'h':
			err = 0;
			goto usage;
		default:
			err = -EINVAL;
			goto usage;
		}
	}

	if (!name || optind != argc) {
		err = -EINVAL;
		goto usage;
	}

	err = vhd_open(&vhd, name, VHD_OPEN_RDONLY);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	if (size)
		printf("%llu\n", vhd.footer.curr_size >> 20);

	if (physize) {
		err = vhd_get_phys_size(&vhd, &currsize);
		if (err)
			printf("failed to get physical size: %d\n", err);
		else
			printf("%llu\n", currsize);
	}

	if (parent) {
		ret = 0;

		if (vhd.footer.type != HD_TYPE_DIFF)
			printf("%s has no parent\n", name);
		else {
			char *pname;

			ret = vhd_parent_locator_get(&vhd, &pname);
			if (ret)
				printf("query failed\n");
			else {
				printf("%s\n", pname);
				free(pname);
			}
		}

		err = (err ? : ret);
	}

	if (fields) {
		int hidden;

		ret = vhd_hidden(&vhd, &hidden);
		if (ret)
			printf("error checking 'hidden' field: %d\n", ret);
		else
			printf("hidden: %d\n", hidden);

		err = (err ? : ret);
	}

	if (depth) {
		int length;

		ret = vhd_chain_depth(&vhd, &length);
		if (ret)
			printf("error checking chain depth: %d\n", ret);
		else
			printf("chain depth: %d\n", length);

		err = (err ? : ret);
	}
		
	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> [-v print virtual size (in MB)] "
	       "[-s print physical utilization (bytes)] [-p print parent] "
	       "[-f print fields] [-d print chain depth] [-h help]\n");
	return err;
}
