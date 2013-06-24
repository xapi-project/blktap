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
#include <assert.h>
#include <stdbool.h>

#include "libvhd.h"

#ifndef ULLONG_MAX
#define ULLONG_MAX (~0ULL)
#endif

int
vhd_init_bitmap(vhd_context_t *ctx, const uint32_t block)
{
	int err;
	void *buf;
	int i;
	int size;

	assert(ctx);

	size = vhd_bytes_padded(ctx->spb >> 3);

	err = posix_memalign(&buf, VHD_SECTOR_SIZE, size);
	if (err)
		return err;

	for (i = 0; i < ctx->spb; i++)
		vhd_bitmap_set(ctx, buf, i);

	err = vhd_write_bitmap(ctx, block, buf);
	free(buf);
	if (err) {
		printf("failed to write bitmap for extent %u: %s\n", block,
				strerror(-err));
		return err;
	}

	return 0;
}

int
vhd_init_bitmaps(vhd_context_t *ctx, const uint32_t from_extent,
		const uint32_t to_extent) {

	unsigned int i;
	int err;

	assert(ctx);
	assert(from_extent <= to_extent);

	for (i = from_extent; i <= to_extent; i++) {
		if (ctx->bat.bat[i] == DD_BLK_UNUSED)
			continue;
		err = vhd_init_bitmap(ctx, i);
		if (err) {
			printf("failed to initialise bitmap for extent %u: %s\n", i,
					strerror(-err));
			return err;
		}
	}

	return 0;
}

int
vhd_io_allocate_blocks_fast(vhd_context_t *ctx, const uint32_t from_extent,
		const uint32_t to_extent, const bool ignore_2tb_limit)
{
	off64_t max;
	int err, gap;
	int i = 0;
	int spp = getpagesize() >> VHD_SECTOR_SHIFT;

	assert(ctx);
	assert(from_extent <= to_extent);

	err = vhd_end_of_data(ctx, &max);
	if (err)
		return err;

	gap   = 0;
	max >>= VHD_SECTOR_SHIFT;

	/* data region of segment should begin on page boundary */
	if ((max + ctx->bm_secs) % spp) {
		gap  = (spp - ((max + ctx->bm_secs) % spp));
		max += gap;
	}

	for (i = from_extent; i <= to_extent; i++) {
		if (max > UINT_MAX && !ignore_2tb_limit) {
			printf("sector offset for extent %u exceeds the 2 TB limit\n", i);
			err = -EOVERFLOW;
			goto out;
		}
		ctx->bat.bat[i] = max;
		max += ctx->bm_secs + ctx->bat.spb;
		if ((max + ctx->bm_secs) % spp) {
			gap = (spp - ((max + ctx->bm_secs) % spp));
			max += gap;
		}
	}
	err = vhd_write_bat(ctx, &ctx->bat);
	if (err)
		goto out;

	err = vhd_init_bitmaps(ctx, from_extent, to_extent);
	if (err)
		printf("failed to initialise bitmaps: %s\n", strerror(-err));

out:
	return err;
}

int
vhd_util_fill(int argc, char **argv)
{
	int err, c;
	char *name;
	void *buf;
	vhd_context_t vhd;
	uint64_t i, sec, secs, from_sector, to_sector;
	int init_bat;
	bool ignore_2tb_limit;

	buf          = NULL;
	name         = NULL;
	init_bat     = 0;
	from_sector  = ULLONG_MAX;
	to_sector    = ULLONG_MAX;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:f:t:bBh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'f':
			from_sector = strtoull(optarg, NULL, 10);
			break;
		case 't':
			to_sector = strtoull(optarg, NULL, 10);
			break;
		case 'b':
			init_bat = 1;
			break;
		case 'B':
			ignore_2tb_limit = true;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || optind != argc)
		goto usage;

	if ((from_sector != ULLONG_MAX || to_sector != ULLONG_MAX) && !init_bat) {
		printf("-f/-t can only be used with -b\n");
		goto usage;
	}

	if (from_sector != ULLONG_MAX && to_sector != ULLONG_MAX) {
		if (to_sector < from_sector) {
			printf("invalid sector range %llu-%llu\n",
					(unsigned long long)from_sector,
					(unsigned long long)to_sector);
			goto usage;
		}
	}

	if (ignore_2tb_limit && !init_bat) {
		printf("-B can only be used with -b\n");
		goto usage;
	}

	err = vhd_open(&vhd, name, VHD_OPEN_RDWR);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	err = vhd_get_bat(&vhd);
	if (err)
		goto done;

	if (init_bat) {
		uint32_t from_extent;
		uint32_t to_extent;

		if (from_sector != ULLONG_MAX)
			from_extent = from_sector / vhd.spb;
		else
			from_extent = 0;
		if (to_sector != ULLONG_MAX)
			to_extent = to_sector / vhd.spb;
		else
			to_extent = vhd.bat.entries;
		err = vhd_io_allocate_blocks_fast(&vhd, from_extent, to_extent,
				ignore_2tb_limit);
		if (err)
			goto done;
	} else {
		err = posix_memalign(&buf, 4096, vhd.header.block_size);
		if (err) {
			err = -err;
			goto done;
		}

		sec = 0;
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
	}

done:
	free(buf);
	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> [-h help] [-b initialise the BAT and bitmaps, "
			"don't write to the data blocks (much faster)] [-f start "
			"intialisation from this sector, only usable with -b] [-t "
			"intialise up to this sector (inclusive), only usable with -b] "
			"[-B ignore the 2 TB limit, only usable with -b]\n");
	return -EINVAL;
}
