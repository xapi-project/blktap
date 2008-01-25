/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */


#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#define TAPDISK
#include "tapdisk.h"
#include "vhd.h"

#define SECTOR_SIZE 512

#define nsize     15
static char nbuf[nsize];

#define secs_round_up(bytes) \
              (((bytes) + (SECTOR_SIZE - 1)) >> SECTOR_SHIFT)
#define secs_round_up_no_zero(bytes) \
              (secs_round_up(bytes) ? : 1)

static inline int
le_test_bit (int nr, volatile u32 *addr)
{
	return (((u32 *)addr)[nr >> 5] >> (nr & 31)) & 1;
}

#define BIT_MASK 0x80

static inline int
be_test_bit (int nr, volatile char *addr)
{
	return ((addr[nr >> 3] << (nr & 7)) & BIT_MASK) != 0;
}

#define test_bit(_info, nr, addr)                                           \
	((_info)->bitmap_format == LITTLE_ENDIAN ?                          \
	 le_test_bit(nr, (uint32_t *)(addr)) : be_test_bit(nr, addr))

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

/* Stringify the VHD timestamp for printing. */
static size_t 
vhd_time_to_s(uint32_t timestamp, char *target)
{
        struct tm tm;
        time_t t1, t2;
        char *cr;
    
        memset(&tm, 0, sizeof(struct tm));
 
        /* VHD uses an epoch of 12:00AM, Jan 1, 2000.         */
        /* Need to adjust this to the expected epoch of 1970. */
        tm.tm_year  = 100;
        tm.tm_mon   = 0;
        tm.tm_mday  = 1;

        t1 = mktime(&tm);
        t2 = t1 + (time_t)timestamp;
        ctime_r(&t2, target);

        /* handle mad ctime_r newline appending. */
        if ((cr = strchr(target, '\n')) != NULL)
		*cr = '\0';

        return (strlen(target));
}

static void
vhd_print_header(struct dd_hdr *h, int hex)
{
	uint32_t  cksm;
	char      uuid[37], time_str[26], cookie[9], out[512], *name;

	printf("VHD Header Summary:\n-------------------\n");

	snprintf(cookie, 9, "%s", h->cookie);
	printf("Cookie              : %s\n", cookie);

	printf("Data offset (unusd) : %s\n", conv(hex, h->data_offset));
	printf("Table offset        : %s\n", conv(hex, h->table_offset));
	printf("Header version      : 0x%08x\n", h->hdr_ver);
	printf("Max BAT size        : %s\n", conv(hex, h->max_bat_size));
	printf("Block size          : %s ", conv(hex, h->block_size));
	printf("(%s MB)\n", conv(hex, h->block_size >> 20));

	name = w2u_decode_location(h->prt_name, out, 512, UTF_16BE);
	printf("Parent name         : %s\n", (name ? : "failed to read name"));
	free(name);

	uuid_unparse(h->prt_uuid, uuid);
	printf("Parent UUID         : %s\n", uuid);
    
	vhd_time_to_s(h->prt_ts, time_str);
	printf("Parent timestamp    : %s\n", time_str);

	cksm = vhd_header_checksum(h);
	printf("Checksum            : 0x%x|0x%x (%s)\n", h->checksum, cksm,
		h->checksum == cksm ? "Good!" : "Bad!");
}

static void
vhd_print_footer(struct hd_ftr *f, int hex)
{
	uint64_t  c, h, s;
	uint32_t  ff_maj, ff_min, cr_maj, cr_min, cksm, cksm_save;
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

	vhd_time_to_s(f->timestamp, time_str);
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
		HD_TYPE_STR[f->type] : "Unknown type!\n");

	cksm = vhd_footer_checksum(f);
	printf("Checksum            : 0x%x|0x%x (%s)\n", f->checksum, cksm,
		f->checksum == cksm ? "Good!" : "Bad!");

	uuid_unparse(f->uuid, uuid);
	printf("UUID                : %s\n", uuid);

	printf("Saved state         : %s\n", f->saved == 0 ? "No" : "Yes");
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
vhd_print_parent(int fd, struct prt_loc *loc)
{
	int i, size;
	char *raw, *out, *name;

	if (loc->code != PLAT_CODE_MACX &&
	    loc->code != PLAT_CODE_W2RU &&
	    loc->code != PLAT_CODE_W2KU)
		return;

	raw  = NULL;
	out  = NULL;
	name = NULL;

	/* data_space *should* be in sectors,
	 * but sometimes we find it in bytes */
	if (loc->data_space < 512)
		size = loc->data_space << VHD_SECTOR_SHIFT;
	else if (loc->data_space % 512 == 0)
		size = loc->data_space;
	else {
		printf("invalid data_space\n");
		return;
	}

	if (lseek64(fd, loc->data_offset, SEEK_SET) == (off64_t)-1) {
		printf("seek to %llu failed: %d\n", loc->data_offset, errno);
		return;
	}

	if (posix_memalign((void **)&raw, 512, size)) {
		printf("out of memory\n");
		return;
	}

	if (read(fd, raw, size) != size) {
		printf("read of parent name failed\n");
		goto done;
	}

	out = malloc(loc->data_len + 1);
	if (!out) {
		printf("out of memory\n");
		goto done;
	}

	switch(loc->code) {
	case PLAT_CODE_MACX:
		name = macx_decode_location(raw, out, loc->data_len);
		break;
	case PLAT_CODE_W2RU:
	case PLAT_CODE_W2KU:
		name = w2u_decode_location(raw, out, loc->data_len, UTF_16LE);
		break;
	}

	if (name)
		printf("       decoded name : %s\n", out);
	else
		printf("       decoding name failed\n");

done:
	free(raw);
	free(out);
	free(name);
}

static void
vhd_print_parent_locators(int fd, struct dd_hdr *hdr, int hex)
{
	int i, n;
	struct prt_loc *loc;

	printf("VHD Parent Locators:\n--------------------\n");

	n = sizeof(hdr->loc) / sizeof(struct prt_loc);
	for (i = 0; i < n; i++) {
		loc = &hdr->loc[i];

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
		vhd_print_parent(fd, loc);
		printf("\n");
	}
}

static void
vhd_print_batmap_header(struct dd_batmap_hdr *hdr, char *map, int hex)
{
	uint32_t cksm;

	printf("VHD Batmap Summary:\n-------------------\n");
	printf("Batmap offset       : %s\n", conv(hex, hdr->batmap_offset));
	printf("Batmap size (secs)  : %s\n", conv(hex, hdr->batmap_size));
	printf("Batmap version      : 0x%08x\n", hdr->batmap_version);

	cksm = vhd_batmap_checksum(hdr, map);
	printf("Checksum            : 0x%x|0x%x (%s)\n", hdr->checksum, cksm,
	       (hdr->checksum == cksm ? "Good!" : "Bad!"));
}

static inline int
check_block_range(struct vhd_info *info, uint64_t block, int hex)
{
	if (block > info->bat_entries) {
		fprintf(stderr, "block %s past end of file\n",
			conv(hex, block));
		return -ERANGE;
	}

	return 0;
}

static int
vhd_read_bitmap(struct vhd_info *info, int fd, uint64_t block, char *buf)
{
	int size, err;
	uint64_t offset;

	if (block > info->bat_entries)
		return -ERANGE;

	size   = info->spb >> 3;
	offset = info->bat[block];

	if (offset == DD_BLK_UNUSED)
		return DD_BLK_UNUSED;

	offset <<= SECTOR_SHIFT;

	if (lseek64(fd, offset, SEEK_SET) == (off64_t)-1)
		return -errno;

	if (read(fd, buf, size) != size)
		return (errno ? -errno : -EIO);

	return 0;
}

static int
vhd_read_batmap(struct disk_driver *dd, struct dd_batmap_hdr *hdr, char **map)
{
	char *buf;
	uint64_t offset;
	int fd, size, err;

	*map = NULL;

	err = vhd_get_batmap_header(dd, hdr);
	if (err)
		return err;

	if (memcmp(hdr->cookie, VHD_BATMAP_COOKIE, 8)) {
		printf("unrecognized batmap cookie\n");
		return -EINVAL;
	}

	size   = hdr->batmap_size << SECTOR_SHIFT;
	offset = hdr->batmap_offset;

	if ((fd = open(dd->name, O_RDONLY | O_LARGEFILE)) == -1)
		return -errno;

	if (lseek64(fd, offset, SEEK_SET) == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	buf = malloc(size);
	if (!buf) {
		err = -ENOMEM;
		goto out;
	}

	if (read(fd, buf, size) != size) {
		free(buf);
		err = (errno ? -errno : -EIO);
		goto out;
	}

	*map = buf;

 out:
	close(fd);
	return 0;
}

static int
vhd_read_data(struct vhd_info *info, int fd, uint64_t block, char *buf)
{
	int size, err;
	uint64_t offset;

	if (block > info->bat_entries)
		return -ERANGE;

	size   = info->spb << SECTOR_SHIFT;
	offset = info->bat[block];

	if (offset == DD_BLK_UNUSED)
		return DD_BLK_UNUSED;

	offset  += secs_round_up_no_zero(info->spb >> 3);
	offset <<= SECTOR_SHIFT;

	if (lseek64(fd, offset, SEEK_SET) == (off64_t)-1)
		return -errno;

	if (read(fd, buf, size) != size)
		return (errno ? -errno : -EIO);

	return 0;
}

static int
vhd_print_headers(struct disk_driver *dd, struct vhd_info *info, int hex)
{
	struct hd_ftr footer;

	vhd_get_footer(dd, &footer);
	vhd_print_footer(&footer, hex);
	printf("\n");

	if (footer.type == HD_TYPE_DYNAMIC || footer.type == HD_TYPE_DIFF) {
		int err;
		char *map;
		struct dd_hdr header;
		struct dd_batmap_hdr map_header;

		err = vhd_get_header(dd, &header);
		if (err) {
			printf("failed to get header\n");
			return err;
		}
		vhd_print_header(&header, hex);
		printf("\n");

		if (footer.type == HD_TYPE_DIFF) {
			int fd;

			fd = open(dd->name, O_RDONLY | O_LARGEFILE);
			if (fd == -1) {
				printf("failed to read parent locators\n");
				return -errno;
			}

			vhd_print_parent_locators(fd, &header, hex);
			printf("\n");
			close(fd);
		}

		err = vhd_read_batmap(dd, &map_header, &map);
		if (err) {
			printf("failed to get batmap header\n");
			return err;
		}
		vhd_print_batmap_header(&map_header, map, hex);
		printf("\n");
		free(map);
	}

	return 0;
}

static int
vhd_print_logical_to_physical(struct disk_driver *dd, struct vhd_info *info,
			      uint64_t sector, int count, int hex)
{
	int i;
	uint32_t blk, lsec;
	uint64_t cur, offset;

	if (sector + count > info->secs) {
		fprintf(stderr, "sector %s past end of file\n",
			conv(hex, sector + count));
			return -ERANGE;
	}

	for (i = 0; i < count; i++) {
		cur    = sector + i;
		blk    = cur / info->spb;
		lsec   = cur % info->spb;
		offset = info->bat[blk];

		if (offset != DD_BLK_UNUSED) {
			offset  += lsec + 1;
			offset <<= SECTOR_SHIFT;
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
vhd_print_bat(struct disk_driver *dd, struct vhd_info *info,
	      uint64_t block, int count, int hex)
{
	int i;
	uint64_t cur, offset;

	if (check_block_range(info, block + count, hex))
		return -ERANGE;

	for (i = 0; i < count; i++) {
		cur    = block + i;
		offset = info->bat[cur];

		printf("block: %s: ", conv(hex, cur));
		printf("offset: %s\n", conv(hex, offset << SECTOR_SHIFT));
	}

	return 0;
}

static int
vhd_print_bitmap(struct disk_driver *dd, struct vhd_info *info,
		 uint64_t block, int count, int hex)
{
	char *buf;
	uint64_t cur;
	int i, fd, err, size;

	if (check_block_range(info, block + count, hex))
		return -ERANGE;

	if ((fd = open(dd->name, O_RDONLY | O_LARGEFILE)) == -1)
		return -errno;

	size = info->spb >> 3;
	buf  = malloc(size);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		cur = block + i;
		err = vhd_read_bitmap(info, fd, cur, buf);

		if (err) {
			if (err == DD_BLK_UNUSED) {
				printf("block %s not allocated\n",
				       conv(hex, cur));
				continue;
			}
			break;
		}

		write(STDOUT_FILENO, buf, size);
	}

	free(buf);
	close(fd);
	return 0;
}

static int
vhd_test_bitmap(struct disk_driver *dd, struct vhd_info *info,
		uint64_t sector, int count, int hex)
{
	char *buf;
	uint64_t cur;
	int i, fd, err, size;
	uint32_t blk, bm_blk, sec;

	if (sector + count > info->secs) {
		printf("sector %s past end of file\n", conv(hex, sector));
		return -ERANGE;
	}

	if ((fd = open(dd->name, O_RDONLY | O_LARGEFILE)) == -1)
		return -errno;

	err    = 0;
	bm_blk = -1;
	size   = info->spb >> 3;

	buf = malloc(size);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		cur = sector + i;
		blk = cur / info->spb;
		sec = cur % info->spb;

		if (blk != bm_blk) {
			bm_blk = blk;
			err    = vhd_read_bitmap(info, fd, blk, buf);
			if (err) {
				if (err == DD_BLK_UNUSED)
					memset(buf, 0, size);
				else
					goto out;
			}
		}

		printf("block %s: ", conv(hex, blk));
		printf("sec: %s: %d\n", conv(hex, sec),
		       test_bit(info, sec, buf));
	}

	err = 0;
 out:
	free(buf);
	close(fd);
	return err;
}

static int
vhd_print_batmap(struct disk_driver *dd, struct vhd_info *info)
{
	char *buf;
	int size, err;
	struct dd_batmap_hdr hdr;

	err = vhd_read_batmap(dd, &hdr, &buf);
	if (err) {
		printf("failed to read batmap: %d\n", err);
		return err;
	}

	size = hdr.batmap_size << SECTOR_SHIFT;
	write(STDOUT_FILENO, buf, size);

	free(buf);
	return 0;
}

static int
vhd_test_batmap(struct disk_driver *dd, struct vhd_info *info,
		uint64_t block, int count, int hex)
{
	char *map;
	int i, err;
	uint64_t cur;
	struct dd_batmap_hdr hdr;

	if (check_block_range(info, block + count, hex))
		return -ERANGE;

	err = vhd_read_batmap(dd, &hdr, &map);
	if (err) {
		fprintf(stderr, "failed to get batmap\n");
		return err;
	}

	for (i = 0; i < count; i++) {
		cur = block + i;
		fprintf(stderr, "batmap for block %s: %d\n", 
			conv(hex, cur), test_bit(info, cur, map));
	}

	free(map);
	return 0;
}

static int
vhd_print_data(struct disk_driver *dd, struct vhd_info *info,
	       uint64_t block, int count, int hex)
{
	char *buf;
	uint64_t cur;
	int i, fd, err, size;

	if (check_block_range(info, block + count, hex))
		return -ERANGE;

	if ((fd = open(dd->name, O_RDONLY | O_LARGEFILE)) == -1)
		return -errno;

	size = info->spb << SECTOR_SHIFT;
	buf  = malloc(size);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		cur = block + i;
		err = vhd_read_data(info, fd, cur, buf);

		if (err) {
			if (err == DD_BLK_UNUSED) {
				printf("block %s not allocated\n",
				       conv(hex, cur));
				continue;
			}
			else
				break;
		}

		write(STDOUT_FILENO, buf, size);
	}

	free(buf);
	close(fd);
	return 0;
}

int
vhd_read(struct disk_driver *dd, int argc, char *argv[])
{
	struct vhd_info info;
	int c, err, headers, hex;
	uint64_t bat, bitmap, tbitmap, batmap, tbatmap, data, lsec, count;

	err     = 0;
	hex     = 0;
	headers = 0;
	count   = 1;
	bat     = -1;
	bitmap  = -1;
	tbitmap = -1;
	batmap  = -1;
	tbatmap = -1;
	data    = -1;
	lsec    = -1;

	while ((c = getopt(argc, argv, "pt:b:m:i:aj:d:c:xh")) != -1) {
		switch(c) {
		case 'p':
			headers = 1;
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
		case 'a':
			batmap = 1;
			break;
		case 'j':
			tbatmap = strtoull(optarg, NULL, 10);
			break;
		case 'd':
			data = strtoull(optarg, NULL, 10);
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

	if (optind != argc - 1)
		goto usage;

	err = vhd_get_bat(dd, &info);
	if (err) {
		printf("Failed to get info for %s\n", dd->name);
		goto out;
	}

	if (headers)
		vhd_print_headers(dd, &info, hex);

	if (lsec != -1) {
		err = vhd_print_logical_to_physical(dd, &info,
						    lsec, count, hex);
		if (err)
			goto out;
	}

	if (bat != -1) {
		err = vhd_print_bat(dd, &info, bat, count, hex);
		if (err)
			goto out;
	}

	if (bitmap != -1) {
		err = vhd_print_bitmap(dd, &info, bitmap, count, hex);
		if (err)
			goto out;
	}

	if (tbitmap != -1) {
		err = vhd_test_bitmap(dd, &info, tbitmap, count, hex);
		if (err)
			goto out;
	}

	if (batmap != -1) {
		err = vhd_print_batmap(dd, &info);
		if (err)
			goto out;
	}

	if (tbatmap != -1) {
		err = vhd_test_batmap(dd, &info, tbatmap, count, hex);
		if (err)
			goto out;
	}

	if (data != -1) {
		err = vhd_print_data(dd, &info, data, count, hex);
		if (err)
			goto out;
	}

 out:
	free(info.bat);
	return err;

 usage:
	fprintf(stderr, "usage: vhd read filename [options]\n"
		"options:\n"
		"-h          help\n"
		"-p          print VHD headers\n"
		"-t sec      translate logical sector to VHD location\n"
		"-b blk      print bat entry\n"
		"-m blk      print bitmap\n"
		"-i sec      test bitmap for logical sector\n"
		"-a          print batmap\n"
		"-j blk      test batmap for block\n"
		"-d blk      print data\n"
		"-c num      num blocks\n"
		"-x          print in hex\n");
	return EINVAL;
}
