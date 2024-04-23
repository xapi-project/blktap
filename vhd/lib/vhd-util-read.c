/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include "libvhd.h"
#include "vhd-util.h"

#define nsize     21
static char nbuf[nsize];

static inline char *
__xconv(uint64_t num)
{
	snprintf(nbuf, nsize, "%#" PRIx64 , num);
	return nbuf;
}

static inline char *
__dconv(uint64_t num)
{
	snprintf(nbuf, nsize, "%" PRIu64, num);
	return nbuf;
}

#define conv(hex, num) \
	(hex ? __xconv((uint64_t)num) : __dconv((uint64_t)num))

static void
vhd_print_header(vhd_context_t *vhd, vhd_header_t *h, int hex)
{
	int err;
	uint32_t  cksm;
	char      uuid[37], time_str[26], cookie[9], *name;

	printf("VHD Header Summary:\n-------------------\n");

	snprintf(cookie, 9, "%s", h->cookie);
	printf("Cookie              : %s\n", cookie);

	printf("Data offset (unusd) : %s\n", conv(hex, h->data_offset));
	printf("Table offset        : %s\n", conv(hex, h->table_offset));
	printf("Header version      : 0x%08x\n", h->hdr_ver);
	printf("Max BAT size        : %s\n", conv(hex, h->max_bat_size));
	printf("Block size          : %s ", conv(hex, h->block_size));
	printf("(%s MB)\n", conv(hex, h->block_size >> 20));

	err = vhd_header_decode_parent(vhd, h, &name);
	printf("Parent name         : %s\n",
	       (err ? "failed to read name" : name));
	free(name);

	uuid_unparse(h->prt_uuid, uuid);
	printf("Parent UUID         : %s\n", uuid);
    
	vhd_time_to_string(h->prt_ts, time_str);
	printf("Parent timestamp    : %s\n", time_str);

	cksm = vhd_checksum_header(h);
	printf("Checksum            : 0x%x|0x%x (%s)\n", h->checksum, cksm,
		h->checksum == cksm ? "Good!" : "Bad!");
	printf("\n");
}

/* String table for hd.type */
char *hd_type_str[7] = {
        "None",                    /* 0 */
        "Reserved (deprecated)",   /* 1 */
        "Fixed hard disk",         /* 2 */
        "Dynamic hard disk",       /* 3 */
        "Differencing hard disk",  /* 4 */
        "Reserved (deprecated)",   /* 5 */
        "Reserved (deprecated)"    /* 6 */
};

static void
vhd_print_footer(vhd_footer_t *f, int hex)
{
	uint64_t  c, h, s;
	uint32_t  ff_maj, ff_min, cr_maj, cr_min, cksm;
	char      time_str[26], creator[5], uuid[37], cookie[9];

	printf("VHD Footer Summary:\n-------------------\n");

	snprintf(cookie, 9, "%s", f->cookie);
	printf("Cookie              : %s\n", cookie);

	printf("Features            : (0x%08x) %s%s\n", f->features,
		(f->features & HD_TEMPORARY) ? "<TEMP>" : "",
		(f->features & HD_RESERVED)  ? "<RESV>" : "");

	ff_maj = f->ff_version >> 16;
	ff_min = f->ff_version & 0xffff;
	printf("File format version : Major: %d, Minor: %d\n", 
		ff_maj, ff_min);

	printf("Data offset         : %s\n", conv(hex, f->data_offset));

	vhd_time_to_string(f->timestamp, time_str);
	printf("Timestamp           : %s\n", time_str);

	memcpy(creator, f->crtr_app, 4);
	creator[4] = '\0';
	printf("Creator Application : '%s'\n", creator);

	cr_maj = f->crtr_ver >> 16;
	cr_min = f->crtr_ver & 0xffff;
	printf("Creator version     : Major: %d, Minor: %d\n",
		cr_maj, cr_min);

	printf("Creator OS          : %s\n",
		((f->crtr_os == HD_CR_OS_WINDOWS) ? "Windows" :
		 ((f->crtr_os == HD_CR_OS_MACINTOSH) ? "Macintosh" : 
		  "Unknown!")));

	printf("Original disk size  : %s MB ", conv(hex, f->orig_size >> 20));
	printf("(%s Bytes)\n", conv(hex, f->orig_size));

	printf("Current disk size   : %s MB ", conv(hex, f->curr_size >> 20));
	printf("(%s Bytes)\n", conv(hex, f->curr_size));

	c = f->geometry >> 16;
	h = (f->geometry & 0x0000FF00) >> 8;
	s = f->geometry & 0x000000FF;
	printf("Geometry            : Cyl: %s, ", conv(hex, c));
	printf("Hds: %s, ", conv(hex, h));
	printf("Sctrs: %s\n", conv(hex, s));
	printf("                    : = %s MB ", conv(hex, (c * h * s) >> 11));
	printf("(%s Bytes)\n", conv(hex, c * h * s << 9));

	printf("Disk type           : %s\n", 
	       f->type <= HD_TYPE_MAX ? 
	       hd_type_str[f->type] : "Unknown type!\n");

	cksm = vhd_checksum_footer(f);
	printf("Checksum            : 0x%x|0x%x (%s)\n", f->checksum, cksm,
		f->checksum == cksm ? "Good!" : "Bad!");

	uuid_unparse(f->uuid, uuid);
	printf("UUID                : %s\n", uuid);

	printf("Saved state         : %s\n", f->saved == 0 ? "No" : "Yes");
	printf("Hidden              : %d\n", f->hidden);
	printf("\n");
}

static inline char *
code_name(uint32_t code)
{
	switch(code) {
	case PLAT_CODE_NONE:
		return "PLAT_CODE_NONE";
	case PLAT_CODE_WI2R:
		return "PLAT_CODE_WI2R";
	case PLAT_CODE_WI2K:
		return "PLAT_CODE_WI2K";
	case PLAT_CODE_W2RU:
		return "PLAT_CODE_W2RU";
	case PLAT_CODE_W2KU:
		return "PLAT_CODE_W2KU";
	case PLAT_CODE_MAC:
		return "PLAT_CODE_MAC";
	case PLAT_CODE_MACX:
		return "PLAT_CODE_MACX";
	default:
		return "UNKOWN";
	}
}

static void
vhd_print_parent(vhd_context_t *vhd, vhd_parent_locator_t *loc)
{
	int err;
	char *buf;

	err = vhd_parent_locator_read(vhd, loc, &buf);
	if (err) {
		printf("failed to read parent name\n");
		return;
	}

	printf("       decoded name : %s\n", buf);
	free(buf);
}

static void
vhd_print_parent_locators(vhd_context_t *vhd, int hex)
{
	int i, n;
	vhd_parent_locator_t *loc;

	printf("VHD Parent Locators:\n--------------------\n");

	n = sizeof(vhd->header.loc) / sizeof(struct prt_loc);
	for (i = 0; i < n; i++) {
		loc = &vhd->header.loc[i];

		if (loc->code == PLAT_CODE_NONE)
			continue;

		printf("locator:            : %d\n", i);
		printf("       code         : %s\n",
		       code_name(loc->code));
		printf("       data_space   : %s\n",
		       conv(hex, loc->data_space));
		printf("       data_length  : %s\n",
		       conv(hex, loc->data_len));
		printf("       data_offset  : %s\n",
		       conv(hex, loc->data_offset));
		vhd_print_parent(vhd, loc);
		printf("\n");
	}
}

static void
vhd_print_keyhash(vhd_context_t *vhd)
{
	int ret;
	struct vhd_keyhash keyhash;

	ret = vhd_get_keyhash(vhd, &keyhash);
	if (ret)
		printf("error reading keyhash: %d\n", ret);
	else if (keyhash.cookie == 1) {
		int i;

		printf("Batmap keyhash nonce: ");
		for (i = 0; i < sizeof(keyhash.nonce); i++)
			printf("%02x", keyhash.nonce[i]);

		printf("\nBatmap keyhash hash : ");
		for (i = 0; i < sizeof(keyhash.hash); i++)
			printf("%02x", keyhash.hash[i]);

		printf("\n");
	}
}

static void
vhd_print_batmap_header(vhd_context_t *vhd, vhd_batmap_t *batmap, int hex)
{
	uint32_t cksm;

	printf("VHD Batmap Summary:\n-------------------\n");
	printf("Batmap offset       : %s\n",
	       conv(hex, batmap->header.batmap_offset));
	printf("Batmap size (secs)  : %s\n",
	       conv(hex, batmap->header.batmap_size));
	printf("Batmap version      : 0x%08x\n",
	       batmap->header.batmap_version);
	vhd_print_keyhash(vhd);

	cksm = vhd_checksum_batmap(vhd, batmap);
	printf("Checksum            : 0x%x|0x%x (%s)\n",
	       batmap->header.checksum, cksm,
	       (batmap->header.checksum == cksm ? "Good!" : "Bad!"));
	printf("\n");
}

static inline int
check_block_range(vhd_context_t *vhd, uint64_t block, int hex)
{
	if (block > vhd->header.max_bat_size) {
		fprintf(stderr, "block %s past end of file\n",
			conv(hex, block));
		return -ERANGE;
	}

	return 0;
}

static int
vhd_print_headers(vhd_context_t *vhd, int hex)
{
	int err;

	vhd_print_footer(&vhd->footer, hex);

	if (vhd_type_dynamic(vhd)) {
		vhd_print_header(vhd, &vhd->header, hex);

		if (vhd->footer.type == HD_TYPE_DIFF)
			vhd_print_parent_locators(vhd, hex);

		if (vhd_has_batmap(vhd)) {
			err = vhd_get_batmap(vhd);
			if (err) {
				printf("failed to get batmap header\n");
				return err;
			}

			vhd_print_batmap_header(vhd, &vhd->batmap, hex);
		}
	}

	return 0;
}

static int
vhd_dump_headers(const char *name, int hex)
{
	vhd_context_t vhd;

	libvhd_set_log_level(1);
	memset(&vhd, 0, sizeof(vhd));

	printf("\n%s appears invalid; dumping headers\n\n", name);

	vhd.fd = open_optional_odirect(name, O_DIRECT | O_LARGEFILE | O_RDONLY);
	if (vhd.fd == -1)
		return -errno;

	vhd.file = strdup(name);

	vhd_read_footer(&vhd, &vhd.footer, false);
	vhd_read_header(&vhd, &vhd.header);

	vhd_print_footer(&vhd.footer, hex);
	vhd_print_header(&vhd, &vhd.header, hex);

	close(vhd.fd);
	free(vhd.file);

	return 0;
}

static int
vhd_print_logical_to_physical(vhd_context_t *vhd,
			      uint64_t sector, int count, int hex)
{
	int i;
	uint32_t blk, lsec;
	uint64_t cur, offset;

	if (vhd_sectors_to_bytes(sector + count) > vhd->footer.curr_size) {
		fprintf(stderr, "sector %s past end of file\n",
			conv(hex, sector + count));
			return -ERANGE;
	}

	for (i = 0; i < count; i++) {
		cur    = sector + i;
		blk    = cur / vhd->spb;
		lsec   = cur % vhd->spb;
		offset = vhd->bat.bat[blk];

		if (offset != DD_BLK_UNUSED) {
			offset += lsec + 1;
			offset  = vhd_sectors_to_bytes(offset);
		}

		printf("logical sector %s: ", conv(hex, cur));
		printf("block number: %s, ", conv(hex, blk));
		printf("sector offset: %s, ", conv(hex, lsec));
		printf("file offset: %s\n", (offset == DD_BLK_UNUSED ?
			"not allocated" : conv(hex, offset)));
	}

	return 0;
}

static int
vhd_print_bat(vhd_context_t *vhd, uint64_t block, int count, int hex)
{
	int i;
	uint64_t cur, offset;

	if (check_block_range(vhd, block + count, hex))
		return -ERANGE;

	for (i = 0; i < count && i < vhd->bat.entries; i++) {
		cur    = block + i;
		offset = vhd->bat.bat[cur];

		printf("block: %s: ", conv(hex, cur));
		printf("offset: %s\n",
		       (offset == DD_BLK_UNUSED ? "not allocated" :
			conv(hex, vhd_sectors_to_bytes(offset))));
	}

	return 0;
}

static int
vhd_print_bat_str(vhd_context_t *vhd)
{
	int i, err, total_blocks, bitmap_size;
	char *bitmap;
	ssize_t n;

	err = 0;

	if (!vhd_type_dynamic(vhd))
		return -EINVAL;

	total_blocks = vhd->footer.curr_size / vhd->header.block_size;
	bitmap_size = total_blocks >> 3;
	if (bitmap_size << 3 < total_blocks)
		bitmap_size++;

	bitmap = malloc(bitmap_size);
	if (!bitmap)
		return -ENOMEM;
	memset(bitmap, 0, bitmap_size);

	for (i = 0; i < total_blocks; i++) {
		if (vhd->bat.bat[i] != DD_BLK_UNUSED)
			set_bit(bitmap, i);
	}

	n = write(STDOUT_FILENO, bitmap, bitmap_size);
	if (n < 0)
		err = -errno;

	free(bitmap);

	return err;
}

static int
vhd_print_bitmap(vhd_context_t *vhd, uint64_t block, int count, int hex)
{
	char *buf;
	int i, err;
	uint64_t cur;
	ssize_t n;

	if (check_block_range(vhd, block + count, hex))
		return -ERANGE;

	for (i = 0; i < count; i++) {
		cur = block + i;

		if (vhd->bat.bat[cur] == DD_BLK_UNUSED) {
			printf("block %s not allocated\n", conv(hex, cur));
			continue;
		}

		err = vhd_read_bitmap(vhd, cur, &buf);
		if (err)
			goto out;

		n = write(STDOUT_FILENO, buf, vhd_sectors_to_bytes(vhd->bm_secs));
		if (n < 0) {
			err = -errno;
			goto out;
		}

		free(buf);
	}

	err = 0;
out:
	return err;
}

static int
vhd_test_bitmap(vhd_context_t *vhd, uint64_t sector, int count, int hex)
{
	char *buf;
	uint64_t cur;
	int i, err, bit;
	uint32_t blk, bm_blk, sec;

	if (vhd_sectors_to_bytes(sector + count) > vhd->footer.curr_size) {
		printf("sector %s past end of file\n", conv(hex, sector));
		return -ERANGE;
	}

	bm_blk = -1;
	buf    = NULL;

	for (i = 0; i < count; i++) {
		cur = sector + i;
		blk = cur / vhd->spb;
		sec = cur % vhd->spb;

		if (blk != bm_blk) {
			bm_blk = blk;
			free(buf);
			buf = NULL;

			if (vhd->bat.bat[blk] != DD_BLK_UNUSED) {
				err = vhd_read_bitmap(vhd, blk, &buf);
				if (err)
					goto out;
			}
		}

		if (vhd->bat.bat[blk] == DD_BLK_UNUSED)
			bit = 0;
		else
			bit = vhd_bitmap_test(vhd, buf, sec);

		printf("block %s: ", conv(hex, blk));
		printf("sec: %s: %d\n", conv(hex, sec), bit);
	}

	err = 0;
 out:
	free(buf);
	return err;
}

static int
vhd_print_bitmap_extents(vhd_context_t *vhd, uint64_t sector, int count,
			 int hex)
{
	char *buf;
	uint64_t cur;
	int i, err, bit;
	uint32_t blk, bm_blk, sec;
	int64_t s, r;

	if (vhd_sectors_to_bytes(sector + count) > vhd->footer.curr_size) {
		printf("sector %s past end of file\n", conv(hex, sector));
		return -ERANGE;
	}

	bm_blk = -1;
	buf    = NULL;
	s = -1;
	r = 0;

	for (i = 0; i < count; i++) {
		cur = sector + i;
		blk = cur / vhd->spb;
		sec = cur % vhd->spb;

		if (blk != bm_blk) {
			bm_blk = blk;
			free(buf);
			buf = NULL;

			if (vhd->bat.bat[blk] != DD_BLK_UNUSED) {
				err = vhd_read_bitmap(vhd, blk, &buf);
				if (err)
					goto out;
			}
		}

		if (vhd->bat.bat[blk] == DD_BLK_UNUSED)
			bit = 0;
		else
			bit = vhd_bitmap_test(vhd, buf, sec);

		if (bit) {
			if (r == 0)
				s = cur;
			r++;
		} else {
			if (r > 0) {
				printf("%s ", conv(hex, s));
				printf("%s\n", conv(hex, r));
			}
			r = 0;
		}
	}
	if (r > 0) {
		printf("%s ", conv(hex, s));
		printf("%s\n", conv(hex, r));
	}

	err = 0;
 out:
	free(buf);
	return err;
}

static int
vhd_print_batmap(vhd_context_t *vhd)
{
	int err, gcc;
	size_t size;

	err = vhd_get_batmap(vhd);
	if (err) {
		printf("failed to read batmap: %d\n", err);
		return err;
	}

	size = vhd_sectors_to_bytes(vhd->batmap.header.batmap_size);
	gcc = write(STDOUT_FILENO, vhd->batmap.map, size);
	if (gcc)
		;

	return 0;
}

static int
vhd_test_batmap(vhd_context_t *vhd, uint64_t block, int count, int hex)
{
	int i, err;
	uint64_t cur;

	if (check_block_range(vhd, block + count, hex))
		return -ERANGE;

	err = vhd_get_batmap(vhd);
	if (err) {
		fprintf(stderr, "failed to get batmap\n");
		return err;
	}

	for (i = 0; i < count; i++) {
		cur = block + i;
		fprintf(stderr, "batmap for block %s: %d\n", conv(hex, cur),
			vhd_batmap_test(vhd, &vhd->batmap, cur));
	}

	return 0;
}

static int
vhd_print_data(vhd_context_t *vhd, uint64_t block, int count, int hex)
{
	char *buf;
	int i, err;
	uint64_t cur;

	err = 0;

	if (check_block_range(vhd, block + count, hex))
		return -ERANGE;

	for (i = 0; i < count; i++) {
		int gcc;
		cur = block + i;

		if (vhd->bat.bat[cur] == DD_BLK_UNUSED) {
			printf("block %s not allocated\n", conv(hex, cur));
			continue;
		}

		err = vhd_read_block(vhd, cur, &buf);
		if (err)
			break;

		gcc = write(STDOUT_FILENO, buf, vhd->header.block_size);
		if (gcc)
			;
		free(buf);
	}

	return err;
}

static int
vhd_read_data(vhd_context_t *vhd, uint64_t sec, int count, int hex)
{
	void *buf;
	uint64_t cur;
	int err, max, secs;

	if (vhd_sectors_to_bytes(sec + count) > vhd->footer.curr_size)
		return -ERANGE;

	max = MIN(vhd_sectors_to_bytes(count), VHD_BLOCK_SIZE);
	err = posix_memalign(&buf, VHD_SECTOR_SIZE, max);
	if (err)
		return -err;

	cur = sec;
	while (count) {
		int gcc;

		secs = MIN((max >> VHD_SECTOR_SHIFT), count);
		err  = vhd_io_read(vhd, buf, cur, secs);
		if (err)
			break;

		gcc = write(STDOUT_FILENO, buf, vhd_sectors_to_bytes(secs));
		if (gcc)
			;

		cur   += secs;
		count -= secs;
	}

	free(buf);
	return err;
}

static int
vhd_read_bytes(vhd_context_t *vhd, uint64_t byte, int count, int hex)
{
	void *buf;
	uint64_t cur;
	int err, max, bytes;

	if (byte + count > vhd->footer.curr_size)
		return -ERANGE;

	max = MIN(count, VHD_BLOCK_SIZE);
	err = posix_memalign(&buf, VHD_SECTOR_SIZE, max);
	if (err)
		return -err;

	cur = byte;
	while (count) {
		ssize_t n;

		bytes = MIN(max, count);
		err   = vhd_io_read_bytes(vhd, buf, bytes, cur);
		if (err)
			break;

		n = write(STDOUT_FILENO, buf, bytes);
		if (n < 0) {
			err = -errno;
			break;
		}

		cur   += bytes;
		count -= bytes;
	}

	free(buf);
	return err;
}

int
vhd_util_read(int argc, char **argv)
{
	char *name;
	vhd_context_t vhd;
	int c, err, headers, hex, bat_str, cache, flags;
	uint64_t bat, bitmap, tbitmap, ebitmap, batmap, tbatmap, data, lsec, count, read;
	uint64_t bread;

	err     = 0;
	hex     = 0;
	cache   = 0;
	headers = 0;
	bat_str = 0;
	count   = 1;
	bat     = -1;
	bitmap  = -1;
	tbitmap = -1;
	ebitmap = -1;
	batmap  = -1;
	tbatmap = -1;
	data    = -1;
	lsec    = -1;
	read    = -1;
	bread   = -1;
	name    = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:pt:b:Bm:i:e:aj:d:c:r:R:xCh")) != -1) {
		switch(c) {
		case 'n':
			name = optarg;
			break;
		case 'p':
			headers = 1;
			break;
		case 'C':
			cache = 1;
			break;
		case 'B':
			bat_str = 1;
			break;
		case 't':
			lsec = strtoul(optarg, NULL, 10);
			break;
		case 'b':
			bat = strtoull(optarg, NULL, 10);
			break;
		case 'm':
			bitmap = strtoull(optarg, NULL, 10);
			break;
		case 'i':
			tbitmap = strtoul(optarg, NULL, 10);
			break;
		case 'e':
			ebitmap = strtoul(optarg, NULL, 10);
			break;
		case 'a':
			batmap = 1;
			break;
		case 'j':
			tbatmap = strtoull(optarg, NULL, 10);
			break;
		case 'd':
			data = strtoull(optarg, NULL, 10);
			break;
		case 'r':
			read = strtoull(optarg, NULL, 10);
			break;
		case 'R':
			bread = strtoull(optarg, NULL, 10);
			break;
		case 'c':
			count = strtoul(optarg, NULL, 10);
			break;
		case 'x':
			hex = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || optind != argc)
		goto usage;

	flags = VHD_OPEN_RDONLY | VHD_OPEN_IGNORE_DISABLED;
	if (cache)
		flags |= VHD_OPEN_CACHED | VHD_OPEN_FAST;
	err = vhd_open(&vhd, name, flags);
	if (err) {
		printf("Failed to open %s: %d\n", name, err);
		vhd_dump_headers(name, hex);
		return err;
	}

	err = vhd_get_bat(&vhd);
	if (err) {
		printf("Failed to get bat for %s: %d\n", name, err);
		goto out;
	}

	if (headers)
		vhd_print_headers(&vhd, hex);

	if (lsec != -1) {
		err = vhd_print_logical_to_physical(&vhd, lsec, count, hex);
		if (err)
			goto out;
	}

	if (bat != -1) {
		err = vhd_print_bat(&vhd, bat, count, hex);
		if (err)
			goto out;
	}

	if (bat_str) {
		err = vhd_print_bat_str(&vhd);
		if (err)
			goto out;
	}

	if (bitmap != -1) {
		err = vhd_print_bitmap(&vhd, bitmap, count, hex);
		if (err)
			goto out;
	}

	if (tbitmap != -1) {
		err = vhd_test_bitmap(&vhd, tbitmap, count, hex);
		if (err)
			goto out;
	}

	if (ebitmap != -1) {
		err = vhd_print_bitmap_extents(&vhd, ebitmap, count, hex);
		if (err)
			goto out;
	}

	if (batmap != -1) {
		err = vhd_print_batmap(&vhd);
		if (err)
			goto out;
	}

	if (tbatmap != -1) {
		err = vhd_test_batmap(&vhd, tbatmap, count, hex);
		if (err)
			goto out;
	}

	if (data != -1) {
		err = vhd_print_data(&vhd, data, count, hex);
		if (err)
			goto out;
	}

	if (read != -1) {
		err = vhd_read_data(&vhd, read, count, hex);
		if (err)
			goto out;
	}

	if (bread != -1) {
		err = vhd_read_bytes(&vhd, bread, count, hex);
		if (err)
			goto out;
	}

	err = 0;

 out:
	vhd_close(&vhd);
	return err;

 usage:
	printf("options:\n"
	       "-h          help\n"
	       "-n          name\n"
	       "-p          print VHD headers\n"
	       "-t sec      translate logical sector to VHD location\n"
	       "-b blk      print bat entry\n"
	       "-B          print entire bat as a bitmap\n"
	       "-m blk      print bitmap\n"
	       "-i sec      test bitmap for logical sector\n"
	       "-e sec      output extent list of allocated logical sectors\n"
	       "-a          print batmap\n"
	       "-j blk      test batmap for block\n"
	       "-d blk      print data\n"
	       "-c num      num units\n"
	       "-r sec      read num sectors at sec\n"
	       "-R byte     read num bytes at byte\n"
	       "-x          print in hex\n");
	return EINVAL;
}
