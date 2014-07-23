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
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

#include "libvhd.h"
#include "canonpath.h"

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

static int
vhd_util_check_depth(const char *name, int *depth)
{
	int err;
	vhd_context_t vhd;

	err = vhd_open(&vhd, name, VHD_OPEN_RDONLY);
	if (err)
		return err;

	err = vhd_chain_depth(&vhd, depth);
	vhd_close(&vhd);

	return err;
}

int
vhd_util_snapshot(int argc, char **argv)
{
	vhd_flag_creat_t flags;
	int c, err, prt_raw, limit, empty_check;
	char *name, *pname, *backing;
	char *ppath, __ppath[PATH_MAX];
	uint64_t size, msize;
	vhd_context_t vhd;
    uint32_t version;

	name        = NULL;
	pname       = NULL;
	ppath       = NULL;
	backing     = NULL;
	size        = 0;
	msize       = 0;
	flags       = 0;
	limit       = 0;
	empty_check = 1;

	if (!argc || !argv) {
		err = -EINVAL;
		goto usage;
	}

	optind = 0;
	while ((c = getopt(argc, argv, "n:p:S:l:meh")) != -1) {

		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'p':
			pname = optarg;
			break;
		case 'S':
			msize = strtoull(optarg, NULL, 10);
		case 'l':
			limit = strtol(optarg, NULL, 10);
			break;
		case 'm':
			vhd_flag_set(flags, VHD_FLAG_CREAT_PARENT_RAW);
			break;
		case 'e':
			empty_check = 0;
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

	ppath = canonpath(pname, __ppath);
	if (!ppath)
		return -errno;

	if (vhd_flag_test(flags, VHD_FLAG_CREAT_PARENT_RAW) || !empty_check) {
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

		/* 
		 * if the sizes of the parent chain are non-uniform, we need to 
		 * pick the right size: that of the supplied parent
		 */
		if (strcmp(ppath, backing)) {
			err = vhd_open(&vhd, ppath, VHD_OPEN_RDONLY);
			if (err)
				goto out;
			size = vhd.footer.curr_size;
            version = vhd.footer.crtr_ver;
			vhd_close(&vhd);
		}

		if (prt_raw)
			vhd_flag_set(flags, VHD_FLAG_CREAT_PARENT_RAW);
	}

	if (limit && !vhd_flag_test(flags, VHD_FLAG_CREAT_PARENT_RAW)) {
		int depth;

		err = vhd_util_check_depth(backing, &depth);
		if (err)
			printf("error checking snapshot depth: %d\n", err);
		else if (depth + 1 > limit) {
			err = -ENOSPC;
			printf("snapshot depth exceeded: "
			       "current depth: %d, limit: %d\n", depth, limit);
		}

		if (err)
			goto out;
	}

	err = vhd_snapshot(name, size, backing, msize << 20, flags);

out:
	free(backing);

	return err;

usage:
	printf("options: <-n name> <-p parent name> [-l snapshot depth limit]"
	       " [-m parent_is_raw] [-S size (MB) for metadata preallocation "
	       "(see vhd-util resize)] [-e link to supplied parent name even "
	       "if it's empty] [-h help]\n");
	return err;
}
