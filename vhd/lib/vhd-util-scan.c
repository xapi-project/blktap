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

#include "list.h"
#include "libvhd.h"

#define VHD_SCAN_FAST        0x01
#define VHD_SCAN_PRETTY      0x02
#define VHD_SCAN_NOFAIL      0x04
#define VHD_SCAN_VERBOSE     0x08

struct vhd_image {
	char                *name;
	char                *parent;
	uint64_t             capacity;
	off64_t              size;
	uint8_t              hidden;
	int                  error;
	char                *message;

	struct list_head     sibling;
	struct list_head     children;
	struct vhd_image    *parent_image;
};

struct vhd_scan {
	int                  cur;
	int                  size;
	struct vhd_image   **images;
};

static int flags;
static struct vhd_scan scan;

static int
vhd_util_scan_pretty_allocate_list(int cnt)
{
	int i;

	scan.images = calloc(cnt, sizeof(struct vhd_image *));
	if (!scan.images)
		return -ENOMEM;

	/* 
	 * allocate each image individually
	 * so qsort won't mess up linked lists
	 */
	for (i = 0; i < cnt; i++) {
		scan.images[i] = calloc(1, sizeof(struct vhd_image));
		if (!scan.images[i])
			goto fail;
	}

	scan.cur  = 0;
	scan.size = cnt;

	return 0;

fail:
	for (i = 0; i < cnt; i++)
		free(scan.images[i]);
	free(scan.images);
	return -ENOMEM;
}

static void
vhd_util_scan_pretty_free_list(void)
{
	int i;

	if (!scan.size)
		return;

	for (i = 0; i < scan.size; i++)
		free(scan.images[i]);

	free(scan.images);
	memset(&scan, 0, sizeof(scan));
}

static int
vhd_util_scan_pretty_add_image(struct vhd_image *image)
{
	int i;
	struct vhd_image *img;

	for (i = 0; i < scan.cur; i++) {
		img = scan.images[i];
		if (!strcmp(img->name, image->name))
			return 0;
	}

	if (scan.cur >= scan.size)
		return -ENOSPC;

	img = scan.images[scan.cur];
	INIT_LIST_HEAD(&img->sibling);
	INIT_LIST_HEAD(&img->children);

	img->capacity = image->capacity;
	img->size     = image->size;
	img->hidden   = image->hidden;
	img->error    = image->error;
	img->message  = image->message;

	img->name = strdup(image->name);
	if (!img->name)
		goto fail;

	if (image->parent) {
		img->parent = strdup(image->parent);
		if (!img->parent)
			goto fail;
	}

	scan.cur++;
	return 0;

fail:
	free(img->name);
	free(img->parent);
	memset(img, 0, sizeof(*img));
	return -ENOMEM;
}

static int
vhd_util_scan_pretty_image_compare(const void *lhs, const void *rhs)
{
	struct vhd_image *l, *r;

	l = *(struct vhd_image **)lhs;
	r = *(struct vhd_image **)rhs;

	return strcmp(l->name, r->name);
}

static void
vhd_util_scan_print_image_indent(struct vhd_image *image, int tab)
{
	char *pad, *name, *pmsg, *parent;

	pad    = (tab ? " " : "");
	name   = image->name;
	parent = (image->parent ? : "none");

	if ((flags & VHD_SCAN_PRETTY) && image->parent && !image->parent_image)
		pmsg = " (not found in scan)";
	else
		pmsg = "";

	if (!(flags & VHD_SCAN_VERBOSE)) {
		name = basename(image->name);
		if (image->parent)
			parent = basename(image->parent);
	}

	if (image->error)
		printf("%*svhd=%s scan-error=%d error-message='%s'\n",
		       tab, pad, image->name, image->error, image->message);
	else
		printf("%*svhd=%s capacity=%"PRIu64" size=%"PRIu64" hidden=%u "
		       "parent=%s%s\n", tab, pad, name, image->capacity,
		       image->size, image->hidden, parent, pmsg);
}

static void
vhd_util_scan_pretty_print_tree(struct vhd_image *image, int depth)
{
	struct vhd_image *img, *tmp;

	vhd_util_scan_print_image_indent(image, depth * 3);

	list_for_each_entry_safe(img, tmp, &image->children, sibling)
		if (!img->hidden)
			vhd_util_scan_pretty_print_tree(img, depth + 1);

	list_for_each_entry_safe(img, tmp, &image->children, sibling)
		if (img->hidden)
			vhd_util_scan_pretty_print_tree(img, depth + 1);

	free(image->name);
	free(image->parent);

	image->name   = NULL;
	image->parent = NULL;
}

static void
vhd_util_scan_pretty_print_images(void)
{
	int i;
	struct vhd_image *image, **parentp, *parent, *keyp, key;

	qsort(scan.images, scan.cur, sizeof(scan.images[0]),
	      vhd_util_scan_pretty_image_compare);

	for (i = 0; i < scan.cur; i++) {
		image = scan.images[i];

		if (!image->parent) {
			image->parent_image = NULL;
			continue;
		}

		memset(&key, 0, sizeof(key));
		key.name = image->parent;
		keyp     = &key;

		parentp  = bsearch(&keyp, scan.images, scan.cur,
				   sizeof(scan.images[0]),
				   vhd_util_scan_pretty_image_compare);
		if (!parentp) {
			image->parent_image = NULL;
			continue;
		}

		parent = *parentp;
		image->parent_image = parent;
		list_add_tail(&image->sibling, &parent->children);
	}

	for (i = 0; i < scan.cur; i++) {
		image = scan.images[i];

		if (image->parent_image)
			continue;

		vhd_util_scan_pretty_print_tree(image, 0);
	}

	for (i = 0; i < scan.cur; i++) {
		image = scan.images[i];

		if (!image->name)
			continue;

		vhd_util_scan_pretty_print_tree(image, 0);
	}
}

static void
vhd_util_scan_print_image(struct vhd_image *image)
{
	int err;

	if (flags & VHD_SCAN_PRETTY) {
		err = vhd_util_scan_pretty_add_image(image);
		if (!err)
			return;

		if (!image->error) {
			image->error   = err;
			image->message = "allocating memory";
		}
	}

	vhd_util_scan_print_image_indent(image, 0);
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

	/*
	if (flags & VHD_SCAN_NOFAIL)
		return 0;
	*/

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

		if (!(flags & VHD_SCAN_PRETTY))
			image.name = files[i];
		else {
			image.name = realpath(files[i], NULL);
			if (!image.name) {
				ret           = -EAGAIN;
				image.name    = files[i];
				image.message = "resolving name";
				image.error   = -errno;
				goto end;
			}
		}

		err = vhd_open(&vhd, image.name, vhd_flags);
		if (err) {
			ret           = -EAGAIN;
			vhd.file      = NULL;
			image.message = "opening file";
			image.error   = err;
			goto end;
		}

		image.capacity = vhd.footer.curr_size;

		image.size = lseek64(vhd.fd, 0, SEEK_END);
		if (image.size == (off64_t)-1) {
			err           = -errno;
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
		if (image.name != files[i])
			free(image.name);
		free(image.parent);

		if (err && !(flags & VHD_SCAN_NOFAIL))
			break;
	}

	if (flags & VHD_SCAN_PRETTY)
		vhd_util_scan_pretty_print_images();

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
	while ((c = getopt(argc, argv, "m:fcpvh")) != -1) {
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
		case 'p':
			flags |= VHD_SCAN_PRETTY;
			break;
		case 'v':
			flags |= VHD_SCAN_VERBOSE;
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

	if (flags & VHD_SCAN_PRETTY)
		flags &= ~VHD_SCAN_FAST;

	if (filter) {
		int gflags = ((flags & VHD_SCAN_FAST) ? GLOB_NOSORT : 0);
		err = glob(filter, gflags, vhd_util_scan_error, &g);
		if (err == GLOB_NOSPACE || err == GLOB_ABORTED) {
			err = (err == GLOB_NOSPACE ? -ENOMEM : -EIO);
			vhd_util_scan_error(filter, err);
			return err;
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

	if (flags & VHD_SCAN_PRETTY) {
		err = vhd_util_scan_pretty_allocate_list(cnt);
		if (err) {
			printf("scan failed: no memory\n");
			return ((flags & VHD_SCAN_NOFAIL) ? EAGAIN : ENOMEM);
		}
	}

	err = vhd_util_scan_files(cnt, files);
	if (err)
		ret = -EAGAIN;

	free(files);
	if (flags & VHD_SCAN_PRETTY)
		vhd_util_scan_pretty_free_list();

	if (flags & VHD_SCAN_NOFAIL)
		return ret;

	return err;

usage:
	printf("usage: [OPTIONS] FILES\n"
	       "options: [-m match filter] [-f fast] [-c continue on failure] "
	       "[-p pretty print] [-v verbose] [-h help]\n");
	return err;
}
