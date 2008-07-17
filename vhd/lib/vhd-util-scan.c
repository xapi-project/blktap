/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#include <glob.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"

#define VHD_SCAN_FAST       0x01
#define VHD_SCAN_PRETTY     0x02
#define VHD_SCAN_VOLUMES    0x04
#define VHD_SCAN_NOFAIL     0x08

struct vhd_image {
	char                *name;
	char                *parent;
	uint64_t             capacity;
	off64_t              size;
	uint8_t              hidden;
	int                  error;
	char                *message;
};

static int flags;

static void
vhd_util_scan_print_image(struct vhd_image *image)
{
	if (image->error)
		printf("vhd=%s scan-error=%d error-message='%s'\n",
		       image->name, image->error, image->message);
	else
		printf("vhd=%s capacity=%llu size=%llu hidden=%u parent=%s\n",
		       image->name, image->capacity, image->size,
		       image->hidden, (image->parent ? : "none"));
}

static int
vhd_util_scan_error(const char *file, int err)
{
	struct vhd_image image;

	memset(&image, 0, sizeof(image));
	image.name    = (char *)file;
	image.error   = err;
	image.message = "failure scanning file";

	vhd_util_scan_print_image(&image);

	if (flags & VHD_SCAN_NOFAIL)
		return 0;

	return err;
}

static int
vhd_util_scan_get_parent(vhd_context_t *vhd, char **parent)
{
	int i, err;
	vhd_parent_locator_t *loc;

	loc     = NULL;
	*parent = NULL;

	if (flags & VHD_SCAN_FAST) {
		err = vhd_header_decode_parent(vhd, &vhd->header, parent);
		if (!err)
			return 0;
	} else {
		/*
		 * vhd_parent_locator_get checks for the existence of the 
		 * parent file. if this call succeeds, all is well; if not,
		 * we'll try to return whatever string we have before failing
		 * outright.
		 */
		err = vhd_parent_locator_get(vhd, parent);
		if (!err)
			return 0;
	}

	for (i = 0; i < 8; i++) {
		if (vhd->header.loc[i].code == PLAT_CODE_MACX) {
			loc = vhd->header.loc + i;
			break;
		}

		if (vhd->header.loc[i].code == PLAT_CODE_W2RU)
			loc = vhd->header.loc + i;

		if (!loc && vhd->header.loc[i].code != PLAT_CODE_NONE)
			loc = vhd->header.loc + i;
	}

	if (!loc)
		return -EINVAL;

	return vhd_parent_locator_read(vhd, loc, parent);
}

static int
vhd_util_scan_files(int cnt, char **files)
{
	vhd_context_t vhd;
	struct vhd_image image;
	int i, ret, err, hidden, vhd_flags;

	ret = 0;
	err = 0;

	vhd_flags = VHD_OPEN_RDONLY;
	if (flags & VHD_SCAN_FAST)
		vhd_flags |= VHD_OPEN_FAST;

	for (i = 0; i < cnt; i++) {
		memset(&vhd, 0, sizeof(vhd));
		memset(&image, 0, sizeof(image));

		image.name = files[i];

		err = vhd_open(&vhd, image.name, vhd_flags);
		if (err) {
			ret           = -EAGAIN;
			vhd.file      = NULL;
			image.message = "opening file";
			image.error   = err;
			goto end;
		}

		image.capacity = vhd.footer.curr_size;

		if (flags & VHD_SCAN_VOLUMES)
			err = vhd_get_phys_size(&vhd, &image.size);
		else {
			image.size = lseek64(vhd.fd, 0, SEEK_END);
			if (image.size == (off64_t)-1)
				err = -errno;
		}

		if (err) {
			ret           = -EAGAIN;
			image.message = "getting physical size";
			image.error   = err;
			goto end;
		}

		err = vhd_hidden(&vhd, &hidden);
		if (err) {
			ret           = -EAGAIN;
			image.message = "checking 'hidden' field";
			image.error   = err;
			goto end;
		}
		image.hidden = hidden;

		if (vhd.footer.type == HD_TYPE_DIFF) {
			err = vhd_util_scan_get_parent(&vhd, &image.parent);
			if (err) {
				ret           = -EAGAIN;
				image.message = "getting parent";
				image.error   = err;
				goto end;
			}
		}

	end:
		vhd_util_scan_print_image(&image);

		if (vhd.file)
			vhd_close(&vhd);
		free(image.parent);

		if (err && !(flags & VHD_SCAN_NOFAIL))
			break;
	}

	if (flags & VHD_SCAN_NOFAIL)
		return ret;

	return err;
}

int
vhd_util_scan(int argc, char **argv)
{
	glob_t g;
	int c, ret, err, cnt;
	char *filter, **files;

	ret    = 0;
	err    = 0;
	flags  = 0;
	filter = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "m:fclph")) != -1) {
		switch (c) {
		case 'm':
			filter = optarg;
			break;
		case 'f':
			flags |= VHD_SCAN_FAST;
			break;
		case 'c':
			flags |= VHD_SCAN_NOFAIL;
			break;
		case 'l':
			flags |= VHD_SCAN_VOLUMES;
			break;
		case 'p':
			printf("pretty scan not yet implemented\n");
			flags |= VHD_SCAN_PRETTY;
			break;
		case 'h':
			goto usage;
		default:
			err = -EINVAL;
			goto usage;
		}
	}

	cnt   = 0;
	files = NULL;
	memset(&g, 0, sizeof(g));

	if (filter) {
		int gflags = ((flags & VHD_SCAN_FAST) ? GLOB_NOSORT : 0);
		err = glob(filter, gflags, vhd_util_scan_error, &g);
		if (err == GLOB_NOSPACE) {
			ret = -EAGAIN;
			vhd_util_scan_error(filter, ENOMEM);
			if (!(flags & VHD_SCAN_NOFAIL))
				return ENOMEM;
		}

		cnt   = g.gl_pathc;
		files = g.gl_pathv;
	}

	cnt  += (argc - optind);
	files = realloc(files, cnt * sizeof(char *));
	if (!files) {
		printf("scan failed: no memory\n");
		return ((flags & VHD_SCAN_NOFAIL) ? EAGAIN : ENOMEM);
	}
	memcpy(files + g.gl_pathc,
	       argv + optind, (argc - optind) * sizeof(char *));

	err = vhd_util_scan_files(cnt, files);
	if (err)
		ret = -EAGAIN;

	free(files);

	if (flags & VHD_SCAN_NOFAIL)
		return ret;

	return err;

usage:
	printf("usage: [OPTIONS] FILES\n"
	       "options: [-m match filter] [-f fast] [-c continue on failure] "
	       "[-l LVM volumes] [-p pretty print] [-h help]\n");
	return err;
}
