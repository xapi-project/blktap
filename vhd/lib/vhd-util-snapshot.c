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
vhd_util_find_snapshot_target(const char *name, char **result, int *parent_raw)
{
	int i, err;
	char *target;
	vhd_context_t vhd;

	*parent_raw = 0;
	*result     = NULL;

	target = strdup(name);
	if (!target)
		return -ENOMEM;

	for (;;) {
		err = vhd_open(&vhd, target, VHD_OPEN_RDONLY);
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

		if (vhd_parent_raw(&vhd)) {
			*parent_raw = 1;
			goto out;
		}

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
	int c, err, prt_raw;
	vhd_flag_creat_t flags;
	char *name, *pname, *ppath, *backing;

	name    = NULL;
	pname   = NULL;
	ppath   = NULL;
	backing = NULL;
	flags   = 0;

	if (!argc || !argv) {
		err = -EINVAL;
		goto usage;
	}

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
			vhd_flag_set(flags, VHD_FLAG_CREAT_FILE_SIZE_FIXED);
			break;
		case 'm':
			vhd_flag_set(flags, VHD_FLAG_CREAT_PARENT_RAW);
			break;
		case 'h':
			err = 0;
			goto usage;
		default:
			err = -EINVAL;
			goto usage;
		}
	}

	if (!name || !pname || optind != argc) {
		err = -EINVAL;
		goto usage;
	}

	ppath = realpath(pname, NULL);
	if (!ppath)
		return -errno;

	if (vhd_flag_test(flags, VHD_FLAG_CREAT_PARENT_RAW)) {
		backing = strdup(ppath);
		if (!backing) {
			err = -ENOMEM;
			goto out;
		}
	} else {
		err = vhd_util_find_snapshot_target(ppath, &backing, &prt_raw);
		if (err) {
			backing = NULL;
			goto out;
		}

		if (prt_raw)
			vhd_flag_set(flags, VHD_FLAG_CREAT_PARENT_RAW);
	}

	err = vhd_snapshot(name, backing, flags);

out:
	free(ppath);
	free(backing);

	return err;

usage:
	printf("options: <-n name> <-p parent name> [-b file_is_fixed_size] "
			"[-m parent_is_raw] [-h help]\n");
	return err;
}
