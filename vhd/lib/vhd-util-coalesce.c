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

static int
vhd_util_coalesce_block(vhd_context_t *vhd,
			vhd_context_t *parent, uint64_t block)
{
	int i, err;
	char *buf, *map;
	uint64_t sec, secs;

	buf = NULL;
	map = NULL;
	sec = block * vhd->spb;

	if (vhd->bat.bat[block] == DD_BLK_UNUSED)
		return 0;

	err = posix_memalign((void **)&buf, 4096, vhd->header.block_size);
	if (err)
		return -err;

	err = vhd_io_read(vhd, buf, sec, vhd->spb);
	if (err)
		goto done;

	if (vhd_has_batmap(vhd) && vhd_batmap_test(vhd, &vhd->batmap, block)) {
		err = vhd_io_write(parent, buf, sec, vhd->spb);
		goto done;
	}

	err = vhd_read_bitmap(vhd, block, &map);
	if (err)
		goto done;

	for (i = 0; i < vhd->spb; i++) {
		if (!vhd_bitmap_test(vhd, map, i))
			continue;

		for (secs = 0; i + secs < vhd->spb; secs++)
			if (!vhd_bitmap_test(vhd, map, i + secs))
				break;

		err = vhd_io_write(parent,
				   buf + (i << VHD_SECTOR_SHIFT),
				   sec + i, secs);
		if (err)
			goto done;

		i += secs;
	}

	err = 0;

done:
	free(buf);
	free(map);
	return err;
}

int
vhd_util_coalesce(int argc, char **argv)
{
	int err, c;
	uint64_t i;
	char *name, *pname;
	vhd_context_t vhd, parent;

	name  = NULL;
	pname = NULL;

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

	err = vhd_open(&vhd, name, O_RDONLY | O_DIRECT);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	err = vhd_parent_locator_get(&vhd, &pname);
	if (err) {
		printf("error finding %s parent: %d\n", name, err);
		vhd_close(&vhd);
		return err;
	}

	err = vhd_open(&parent, pname, O_RDWR | O_DIRECT);
	if (err) {
		printf("error opening %s: %d\n", pname, err);
		free(pname);
		vhd_close(&vhd);
		return err;
	}

	err = vhd_get_bat(&vhd);
	if (err)
		goto done;

	if (vhd_has_batmap(&vhd)) {
		err = vhd_get_batmap(&vhd);
		if (err)
			goto done;
	}

	for (i = 0; i < vhd.bat.entries; i++) {
		err = vhd_util_coalesce_block(&vhd, &parent, i);
		if (err)
			goto done;
	}

	err = 0;

 done:
	free(pname);
	vhd_close(&vhd);
	vhd_close(&parent);
	return err;

usage:
	printf("options: <-n name> [-h help]\n");
	return -EINVAL;
}
