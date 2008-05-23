/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 *
 * Before updating a VHD file, we create a journal consisting of:
 *   - all data at the beginning of the file, up to and including the BAT
 *   - each allocated bitmap (existing at the same offset in the journal as
 *                            its corresponding bitmap in the original file)
 * Updates are performed in place by writing appropriately 
 * transformed versions of journaled bitmaps to the original file.
 */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <endian.h>
#include <byteswap.h>

#define TAPDISK
#include "vhd.h"
#include "tapdisk.h"
#include "atomicio.h"

#if BYTE_ORDER == LITTLE_ENDIAN
  #define BE32_IN(foo)  (*(foo)) = bswap_32(*(foo))
  #define BE64_IN(foo)  (*(foo)) = bswap_64(*(foo))
  #define BE32_OUT(foo) (*(foo)) = bswap_32(*(foo))
  #define BE64_OUT(foo) (*(foo)) = bswap_64(*(foo))
#else
  #define BE32_IN(foo)
  #define BE64_IN(foo)
  #define BE32_OUT(foo)
  #define BE64_OUT(foo)
#endif

#define SECTOR_SIZE         512
#define SECTOR_SHIFT        9
#define VHD_UPDATE_POISON   0xFFFFFFFF

#define round_up(bytes) \
        (((bytes) + (SECTOR_SIZE - 1)) & ~(SECTOR_SIZE - 1))
#define round_up_no_zero(bytes) \
        (round_up((bytes)) ? : 1)

struct vhd_update_ctx {
	char           *file;
	char           *journal;
	struct hd_ftr   footer;
	struct dd_hdr   header;
	struct vhd_info info;
};

static void
usage(void)
{
	printf("usage: vhd-update <-f file> [-j existing journal] [-h]\n");
	exit(EINVAL);
}

static int
load_disk_driver(struct disk_driver *dd, char *file)
{
	int err;

	memset(dd, 0, sizeof(struct disk_driver));

	if (strnlen(file, MAX_NAME_LEN) >= MAX_NAME_LEN)
		return -ENAMETOOLONG;

	dd->drv      = dtypes[DISK_TYPE_VHD]->drv;
	dd->name     = file;
	dd->private  = malloc(dd->drv->private_data_size);
	dd->td_state = malloc(sizeof(struct td_state));

	if (!dd->td_state || !dd->private) {
		err = -ENOMEM;
		goto fail;
	}

	err = dd->drv->td_open(dd, file, TD_OPEN_QUERY);
	if (err)
		goto fail;

	return 0;

 fail:
	free(dd->private);
	free(dd->td_state);
	return -err;
}

static void
close_disk_driver(struct disk_driver *dd)
{
	dd->drv->td_close(dd);
	free(dd->private);
	free(dd->td_state);
}

static int
namedup(char **dest, char *name)
{
	if (strnlen(name, MAX_NAME_LEN) >= MAX_NAME_LEN)
		return -ENAMETOOLONG;

	*dest = strdup(name);
	if (!(*dest))
		return -ENOMEM;

	return 0;
}

static int
init_update_ctx(struct vhd_update_ctx *ctx, char *file)
{
	int err;
	struct disk_driver dd;

	err = namedup(&ctx->file, file);
	if (err)
		return err;

	err = load_disk_driver(&dd, file);
	if (err) {
		printf("failed to open %s: %d\n", file, err);
		return err;
	}

	err = _vhd_get_bat(&dd, &ctx->info);
	if (err)
		printf("failed to read VHD metadata: %d\n", err);
	else {
		_vhd_get_header(&dd, &ctx->header);
		_vhd_get_footer(&dd, &ctx->footer);
	}

	close_disk_driver(&dd);
	return err;
}

static void
free_update_ctx(struct vhd_update_ctx *ctx)
{
	free(ctx->info.bat);
	free(ctx->journal);
	free(ctx->file);
}

static void
vhd_footer_out(struct hd_ftr *footer)
{
	footer->checksum = vhd_footer_checksum(footer);

	BE32_OUT(&footer->features);
	BE32_OUT(&footer->ff_version);
	BE64_OUT(&footer->data_offset);
	BE32_OUT(&footer->timestamp);
	BE32_OUT(&footer->crtr_ver);
	BE32_OUT(&footer->crtr_os);
	BE64_OUT(&footer->orig_size);
	BE64_OUT(&footer->curr_size);
	BE32_OUT(&footer->geometry);
	BE32_OUT(&footer->type);
	BE32_OUT(&footer->checksum);
}

static void
vhd_header_out(struct dd_hdr *header)
{
	int i, n;

	header->checksum = vhd_header_checksum(header);

	BE64_OUT(&header->data_offset);
	BE64_OUT(&header->table_offset);
	BE32_OUT(&header->hdr_ver);
	BE32_OUT(&header->max_bat_size);	
	BE32_OUT(&header->block_size);
	BE32_OUT(&header->checksum);
	BE32_OUT(&header->prt_ts);

	n = sizeof(header->loc) / sizeof(struct prt_loc);
	for (i = 0; i < n; i++) {
		BE32_OUT(&header->loc[i].code);
		BE32_OUT(&header->loc[i].data_space);
		BE32_OUT(&header->loc[i].data_len);
		BE64_OUT(&header->loc[i].data_offset);
	}
}

/*
 * write faulty header version to vhd file so tapdisk won't load it
 */
static int
disable_vhd(struct vhd_update_ctx *ctx)
{
	char *buf;
	int fd, err;

	err = posix_memalign((void **)&buf, 512, sizeof(struct dd_hdr));
	if (err)
		return -err;

	fd = open(ctx->file, O_WRONLY | O_DIRECT);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	if (lseek64(fd, ctx->footer.data_offset, SEEK_SET) == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	memcpy(buf, &ctx->header, sizeof(struct dd_hdr));
	((struct dd_hdr *)buf)->hdr_ver = VHD_UPDATE_POISON;
	vhd_header_out((struct dd_hdr *)buf);

	err = atomicio(vwrite, fd, buf, sizeof(struct dd_hdr));
	if (err != sizeof(struct dd_hdr))
		err = -errno;

	err = 0;

 out:
	free(buf);
	if (fd != -1)
		close(fd);
	return err;
}

/*
 * restore valid header version to vhd file
 */
static int
enable_vhd(struct vhd_update_ctx *ctx)
{
	char *buf;
	int fd, err;

	err = posix_memalign((void **)&buf, 512, sizeof(struct dd_hdr));
	if (err)
		return -err;

	fd = open(ctx->file, O_WRONLY | O_DIRECT);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	if (lseek64(fd, ctx->footer.data_offset, SEEK_SET) == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	memcpy(buf, &ctx->header, sizeof(struct dd_hdr));
	((struct dd_hdr *)buf)->hdr_ver = DD_VERSION;
	vhd_header_out((struct dd_hdr *)buf);

	err = atomicio(vwrite, fd, buf, sizeof(struct dd_hdr));
	if (err != sizeof(struct dd_hdr)) {
		err = -errno;
		goto out;
	}

	err = 0;

 out:
	free(buf);
	if (fd != -1)
		close(fd);
	return err;
}

static inline int
vhd_disabled(struct vhd_update_ctx *ctx)
{
	return (ctx->header.hdr_ver == VHD_UPDATE_POISON);
}

/*
 * update vhd creator version to reflect its new bitmap ordering
 */
static int
update_creator_version(struct vhd_update_ctx *ctx)
{
	char *buf;
	int fd, err;
	off64_t end;

	err = posix_memalign((void **)&buf, 512, sizeof(struct hd_ftr));
	if (err)
		return -err;

	fd = open(ctx->file, O_WRONLY | O_DIRECT | O_LARGEFILE);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	memcpy(buf, &ctx->footer, sizeof(struct hd_ftr));
	((struct hd_ftr *)buf)->crtr_ver = VHD_VERSION(1, 1);
	vhd_footer_out((struct hd_ftr *)buf);

	err = atomicio(vwrite, fd, buf, sizeof(struct hd_ftr));
	if (err != sizeof(struct hd_ftr)) {
		err = -errno;
		goto out;
	}

	if ((end = lseek64(fd, 0, SEEK_END)) == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	if (lseek64(fd, 
		    (end - sizeof(struct hd_ftr)), SEEK_SET) == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	err = atomicio(vwrite, fd, buf, sizeof(struct hd_ftr));
	if (err != sizeof(struct hd_ftr)) {
		err = -errno;
		goto out;
	}

	err = 0;

 out:
	free(buf);
	if (fd != -1)
		close(fd);
	return err;
}

static int
create_journal(struct vhd_update_ctx *ctx)
{
	size_t size;
	off64_t off;
	int i, fd, jd, err;
	char *buf, *journal;

	fd  = -1;
	jd  = -1;
	buf = NULL;

	if (asprintf(&journal, "%s.journal", ctx->file) == -1)
		return -ENOMEM;

	fd = open(ctx->file, O_LARGEFILE | O_RDONLY | O_DIRECT);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	jd = open(journal, 
		  O_CREAT | O_TRUNC | O_LARGEFILE | O_WRONLY | O_DIRECT, 0644);
	if (jd == -1) {
		err = -errno;
		goto out;
	}

	size = round_up_no_zero(ctx->header.table_offset + 
				(ctx->header.max_bat_size * sizeof(uint32_t)));
	err  = posix_memalign((void **)&buf, 512, size);
	if (err) {
		buf = NULL;
		goto out;
	}

	/* copy header, copy of footer, parent locators, and bat to journal */
	if (atomicio(read, fd, buf, size) != size) {
		err = -errno;
		goto out;
	}

	if (atomicio(vwrite, jd, buf, size) != size) {
		err = -errno;
		goto out;
	}

	/* copy bitmaps to journal */
	free(buf);
	size = round_up_no_zero(ctx->info.spb / 8);
	err  = posix_memalign((void **)&buf, 512, size);
	if (err) {
		buf = NULL;
		goto out;
	}

	for (i = 0; i < ctx->info.bat_entries; i++) {
		off = ctx->info.bat[i];

		if (off == DD_BLK_UNUSED)
			continue;

		off <<= SECTOR_SHIFT;

		if (lseek64(fd, off, SEEK_SET) == (off64_t)-1) {
			err = -errno;
			goto out;
		}

		if (lseek64(jd, off, SEEK_SET) == (off64_t)-1) {
			err = -errno;
			goto out;
		}

		if (atomicio(read, fd, buf, size) != size) {
			err = -errno;
			goto out;
		}

		if (atomicio(vwrite, jd, buf, size) != size) {
			err = -errno;
			goto out;
		}
	}

	err = 0;

 out:
	if (fd != -1)
		close(fd);
	if (jd != -1) {
		fchmod(jd, 0400);
		fsync(jd);
		close(jd);
	}
	free(buf);

	if (err)
		free(journal);
	else
		ctx->journal = journal;

	return err;
}

static int
validate_journal(struct vhd_update_ctx *ctx)
{
	size_t size;
	char *fbuf, *jbuf;
	int i, fd, jd, err;
	struct hd_ftr *footer;
	struct dd_hdr *header;

	fd   = -1;
	jd   = -1;
	fbuf = NULL;
	jbuf = NULL;

	fd = open(ctx->file, O_LARGEFILE | O_DIRECT | O_RDONLY);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	jd = open(ctx->journal, O_LARGEFILE | O_DIRECT | O_RDONLY);
	if (jd == -1) {
		err = -errno;
		goto out;
	}

	/* check metadata */
	size = ctx->header.table_offset;
	err  = posix_memalign((void **)&fbuf, 512, size);
	if (err) {
		fbuf = NULL;
		goto out;
	}

	err  = posix_memalign((void **)&jbuf, 512, size);
	if (err) {
		jbuf = NULL;
		goto out;
	}

	if (atomicio(read, fd, fbuf, size) != size) {
		err = -errno;
		goto out;
	}

	if (atomicio(read, jd, jbuf, size) != size) {
		err = -errno;
		goto out;
	}

	/* ignore crtr_ver, ftr_cksum, hdr_ver, hdr_cksum */
	footer           = (struct hd_ftr *)fbuf;
	footer->crtr_ver = 0x00000000;
	footer->checksum = 0x00000000;
	footer           = (struct hd_ftr *)jbuf;
	footer->crtr_ver = 0x00000000;
	footer->checksum = 0x00000000;
	header           = (struct dd_hdr *)(fbuf + ctx->footer.data_offset);
	header->hdr_ver  = 0x00000000;
	header->checksum = 0x00000000;
	header           = (struct dd_hdr *)(jbuf + ctx->footer.data_offset);
	header->hdr_ver  = 0x00000000;
	header->checksum = 0x00000000;

	if (memcmp(fbuf, jbuf, size)) {
		printf("ERROR: journal metadata does not match file\n");
		err = -EINVAL;
		goto out;
	}

	/* check bat entries */
	free(jbuf);
	size = round_up_no_zero(ctx->info.bat_entries * sizeof(uint32_t));
	err  = posix_memalign((void **)&jbuf, 512, size);
	if (err) {
		jbuf = NULL;
		goto out;
	}

	if (atomicio(read, jd, jbuf, size) != size) {
		err = -errno;
		goto out;
	}

	for (i = 0; i < ctx->info.bat_entries; i++) {
		if (ctx->info.bat[i] != (BE32_IN(((uint32_t *)jbuf) + i))) {
			printf("ERROR: journal BAT does not match file\n");
			err = -EINVAL;
			goto out;
		}
	}

	err = 0;
 out:
	if (fd != -1)
		close(fd);
	if (jd != -1)
		close(jd);
	free(fbuf);
	free(jbuf);

	return err;
}

static void
remove_journal(struct vhd_update_ctx *ctx)
{
	unlink(ctx->journal);
}

/*
 * older VHD bitmaps were little endian
 * and bits within a word were set from right to left
 */
static inline int
old_test_bit(int nr, volatile void * addr)
{
        return (((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] >>
                (nr % (sizeof(unsigned long)*8))) & 1;
}

/*
 * new VHD bitmaps are big endian
 * and bits within a word are set from left to right
 */
#define BIT_MASK 0x80
static inline void
new_set_bit (int nr, volatile char *addr)
{
        addr[nr >> 3] |= (BIT_MASK >> (nr & 7));
}

static void
convert_bitmap(char *in, char *out, int bytes)
{
	int i;

	memset(out, 0, bytes);

	for (i = 0; i < bytes << 3; i++)
		if (old_test_bit(i, (void *)in))
			new_set_bit(i, out);
}

static int
update_vhd(struct vhd_update_ctx *ctx, int rollback)
{
	size_t size;
	off64_t off;
	uint64_t sec;
	int i, fd, jd, err;
	char *buf, *converted;

	fd        = -1;
	jd        = -1;
	buf       = NULL;
	converted = NULL;

	fd = open(ctx->file, O_LARGEFILE | O_WRONLY | O_DIRECT);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	jd = open(ctx->journal, O_LARGEFILE | O_RDONLY | O_DIRECT);
	if (jd == -1) {
		err = -errno;
		goto out;
	}

	size = round_up_no_zero(ctx->info.spb / 8);
	err  = posix_memalign((void **)&buf, 512, size);
	if (err) {
		buf = NULL;
		goto out;
	}

	err  = posix_memalign((void **)&converted, 512, size);
	if (err) {
		converted = NULL;
		goto out;
	}

	for (i = 0; i < ctx->info.bat_entries; i++) {
		sec = ctx->info.bat[i];

		if (sec == DD_BLK_UNUSED)
			continue;

		off = sec << SECTOR_SHIFT;
		if (lseek64(jd, off, SEEK_SET) == (off64_t)-1) {
			err = -errno;
			goto out;
		}

		if (lseek64(fd, off, SEEK_SET) == (off64_t)-1) {
			err = -errno;
			goto out;
		}

		if (atomicio(read, jd, buf, size) != size) {
			err = -errno;
			goto out;
		}

		if (rollback)
			memcpy(converted, buf, size);
		else
			convert_bitmap(buf, converted, size);

		if (atomicio(vwrite, fd, converted, size) != size) {
			err = -errno;
			goto out;
		}
	}

	err = 0;
 out:
	if (fd != -1)
		close(fd);
	if (jd != -1)
		close(jd);
	free(buf);

	return err;
}

int
main(int argc, char **argv)
{
	int c, err, rollback;
	char *file, *journal;
	struct vhd_update_ctx ctx;

	file     = NULL;
	journal  = NULL;
	rollback = 0;
	memset(&ctx, 0, sizeof(struct vhd_update_ctx));

	while ((c = getopt(argc, argv, "f:j:rh")) != -1) {
		switch(c) {
		case 'f':
			file = optarg;
			break;
		case 'j':
			journal = optarg;
			break;
		case 'r':
			/* add a rollback option for debugging which
			 * pushes journalled bitmaps to original file
			 * without transforming them */
			rollback = 1;
			break;
		default:
			usage();
		}
	}

	if (!file)
		usage();

	if (rollback && !journal) {
		printf("rollback requires a journal argument\n");
		usage();
	}

	err = init_update_ctx(&ctx, file);
	if (err)
		goto out;

	if (ctx.footer.type == HD_TYPE_FIXED)
		goto out;

	if (ctx.footer.crtr_ver != VHD_VERSION(0, 1)) {
		if (ctx.footer.crtr_ver == VHD_VERSION(1, 1) &&
		    vhd_disabled(&ctx)) {
			err = enable_vhd(&ctx);
			if (err)
				printf("failed to enable VHD: %d\n", err);
		}
		goto out;
	}

	if (journal) {
		err = namedup(&ctx.journal, journal);
		if (err)
			goto out;

		err = validate_journal(&ctx);
		if (err) {
			printf("invalid journal: %d\n", err);
			goto out;
		}
	} else {
		err = create_journal(&ctx);
		if (err) {
			printf("failed to create VHD update journal: %d\n",
			       err);
			goto out;
		}
	}

	err = disable_vhd(&ctx);
	if (err) {
		printf("failed to disable VHD: %d\n", err);
		goto out;
	}

	err = update_vhd(&ctx, rollback);
	if (err) {
		printf("update failed: %d; saving journal\n", err);
		goto out;
	}

	err = update_creator_version(&ctx);
	if (err) {
		printf("failed to udpate creator version: %d\n", err);
		goto out;
	}

	err = enable_vhd(&ctx);
	if (err) {
		printf("failed to enable VHD: %d\n", err);
		goto out;
	}

	remove_journal(&ctx);
	err = 0;

 out:
	free_update_ctx(&ctx);
	return err;
}
