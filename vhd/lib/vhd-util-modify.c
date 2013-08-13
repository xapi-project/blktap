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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

#include "libvhd.h"
#include "canonpath.h"

TEST_FAIL_EXTERN_VARS;

static int
vhd_util_zero_bat(vhd_context_t *vhd)
{
	int err, map_bytes;
	uint64_t i;

	err = vhd_get_bat(vhd);
	if (err)
		return err;

	if (vhd_has_batmap(vhd)) {
		err = vhd_get_batmap(vhd);
		if (err)
			return err;
	}

	for (i = 0; i < vhd->bat.entries; i++)
		vhd->bat.bat[i] = DD_BLK_UNUSED;
	err = vhd_write_bat(vhd, &vhd->bat);
	if (err)
		return err;

	map_bytes = ((vhd->footer.curr_size >> VHD_SECTOR_SHIFT) /
			vhd->spb) >> 3;
	map_bytes = vhd_sectors_to_bytes(secs_round_up_no_zero(map_bytes));
	memset(vhd->batmap.map, 0, map_bytes);
	return vhd_write_batmap(vhd, &vhd->batmap);
}

int
vhd_util_modify(int argc, char **argv)
{
	char *name;
	vhd_context_t vhd;
	int err, c, size, parent, parent_raw, kill_data;
	off64_t newsize = 0;
	char *newparent = NULL;
	char *cpath, __cpath[PATH_MAX];

	name       = NULL;
	size       = 0;
	parent     = 0;
	parent_raw = 0;
	kill_data  = 0;

	optind = 0;
	while ((c = getopt(argc, argv, "n:s:p:mzh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 's':
			size = 1;
			errno = 0;
			newsize = strtoll(optarg, NULL, 10);
			if (errno) {
				fprintf(stderr, "Invalid size '%s'\n", optarg);
				goto usage;
			}
			break;
		case 'p':
			parent = 1;
			newparent = optarg;
			break;
		case 'm':
			parent_raw = 1;
			break;
		case 'z':
			kill_data = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || optind != argc)
		goto usage;

	err = vhd_open(&vhd, name, VHD_OPEN_RDWR);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	if (kill_data) {
		if (vhd_type_dynamic(&vhd))
			err = vhd_util_zero_bat(&vhd);
		else
			err = -ENOSYS;

		if (!err && !vhd.is_block) // truncate file-based VHDs
			err = vhd_write_footer(&vhd, &vhd.footer);

		if (err)
			printf("failed to zero VHD: %d\n", err);
	}

	if (size) {
		err = vhd_set_phys_size(&vhd, newsize);
		if (err)
			printf("failed to set physical size to %"PRIu64":"
			       " %d\n", newsize, err);
	}

	if (parent) {
		TEST_FAIL_AT(FAIL_REPARENT_BEGIN);
		cpath = canonpath(newparent, __cpath);
		err = vhd_change_parent(&vhd, cpath, parent_raw);
		if (err) {
			printf("failed to set parent to '%s': %d\n",
					cpath, err);
			goto done;
		}
		TEST_FAIL_AT(FAIL_REPARENT_END);
	}

done:
	vhd_close(&vhd);
	return err;

usage:
	printf("*** Dangerous operations, use with care ***\n");
	printf("options: <-n name> [-p NEW_PARENT set parent [-m raw]] "
			"[-s NEW_SIZE set size] [-z zero (kill data)] "
			"[-h help]\n");
	return -EINVAL;
}
