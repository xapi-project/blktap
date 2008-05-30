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
	int fixedsize, rawparent;
	char *name, *pname, *backing;

	name  = NULL;
	pname = NULL;
	fixedsize = 0;
	rawparent = 0;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:p:bmh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'p':
			pname = optarg;
			break;
		case 'b':
			fixedsize = 1;
			break;
		case 'm':
			rawparent = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || !pname || optind != argc)
		goto usage;

	if (rawparent)
		backing = strdup(pname);
	else {
		err = vhd_util_find_snapshot_target(pname, &backing);
		if (err)
			return err;
	}

	if (fixedsize && rawparent)
		return vhd_snapshot_fixed_raw(name, backing);
	else if (fixedsize)
		return vhd_snapshot_fixed(name, backing);
	else if (rawparent)
		return vhd_snapshot_raw(name, backing);
	else
		return vhd_snapshot(name, backing);

usage:
	printf("options: <-n name> <-p parent name> [-b file_is_fixed_size] "
			"[-m parent_is_raw] [-h help]\n");
	return -EINVAL;
}
