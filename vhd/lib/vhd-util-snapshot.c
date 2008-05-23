/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"

static int
vhd_util_find_snapshot_target(const char *name, char **result)
{
	int i, err;
	char *target;
	vhd_context_t vhd;

	*result = NULL;
	target  = strdup(name);

	for (;;) {
		err = vhd_open(&vhd, target, O_RDONLY | O_DIRECT);
		if (err)
			return err;

		if (vhd.footer.type != HD_TYPE_DIFF)
			goto out;

		err = vhd_get_bat(&vhd);
		if (err)
			goto out;

		for (i = 0; i < vhd.bat.entries; i++)
			if (vhd.bat.bat[i] != DD_BLK_UNUSED)
				goto out;

		free(target);
		err = vhd_parent_locator_get(&vhd, &target);
		if (err)
			goto out;

		vhd_close(&vhd);
	}

out:
	vhd_close(&vhd);
	if (err)
		free(target);
	else
		*result = target;

	return err;
}

int
vhd_util_snapshot(int argc, char **argv)
{
	int c, err;
	char *name, *pname, *backing;

	name  = NULL;
	pname = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:p:h")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'p':
			pname = optarg;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || !pname || optind != argc)
		goto usage;

	err = vhd_util_find_snapshot_target(pname, &backing);
	if (err)
		return err;

	return vhd_snapshot(name, backing);

usage:
	printf("options: <-n name> <-p parent name> [-h help]\n");
	return -EINVAL;
}
