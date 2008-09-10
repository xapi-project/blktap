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
#include <fnmatch.h>

#include "list.h"
#include "libvhd.h"
#include "lvm-util.h"

#define VHD_SCAN_FAST        0x01
#define VHD_SCAN_PRETTY      0x02
#define VHD_SCAN_VOLUME      0x04
#define VHD_SCAN_NOFAIL      0x08
#define VHD_SCAN_VERBOSE     0x10

struct target {
	char                 name[VHD_MAX_NAME_LEN];
	char                 device[VHD_MAX_NAME_LEN];
	uint64_t             size;
	uint64_t             start;
	uint64_t             end;
};

struct vhd_image {
	char                *name;
	char                *parent;
	uint64_t             capacity;
	off64_t              size;
	uint8_t              hidden;
	int                  error;
	char                *message;

	struct target       *target;

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

		if (image->parent_image || !image->hidden)
			continue;

		vhd_util_scan_pretty_print_tree(image, 0);
	}

	for (i = 0; i < scan.cur; i++) {
		image = scan.images[i];

		if (!image->name || image->parent_image)
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

	if (!image->error && (flags & VHD_SCAN_PRETTY)) {
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

static vhd_parent_locator_t *
vhd_util_scan_get_parent_locator(vhd_context_t *vhd)
{
	int i;
	vhd_parent_locator_t *loc;

	loc = NULL;

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

	return loc;
}

/* 
 * noop for now
 */
static int
vhd_util_scan_extract_volume_name(struct vhd_image *image)
{
	return 0;
}

static int
vhd_util_scan_get_volume_parent(vhd_context_t *vhd, struct vhd_image *image)
{
	int err;
	vhd_parent_locator_t *loc, copy;

	if (flags & VHD_SCAN_FAST) {
		err = vhd_header_decode_parent(vhd,
					       &vhd->header, &image->parent);
		if (!err)
			goto found;
	}

	loc = vhd_util_scan_get_parent_locator(vhd);
	if (!loc)
		return -EINVAL;

	copy = *loc;
	copy.data_offset += image->target->start;
	err = vhd_parent_locator_read(vhd, &copy, &image->parent);
	if (err)
		return err;

found:
	if (!(flags & VHD_SCAN_PRETTY))
		return 0;

	return vhd_util_scan_extract_volume_name(image);
}

static int
vhd_util_scan_get_parent(vhd_context_t *vhd, struct vhd_image *image)
{
	int i, err;
	vhd_parent_locator_t *loc;

	loc = NULL;

	if (flags & VHD_SCAN_VOLUME)
		return vhd_util_scan_get_volume_parent(vhd, image);

	if (flags & VHD_SCAN_FAST) {
		err = vhd_header_decode_parent(vhd,
					       &vhd->header, &image->parent);
		if (!err)
			return 0;
	} else {
		/*
		 * vhd_parent_locator_get checks for the existence of the 
		 * parent file. if this call succeeds, all is well; if not,
		 * we'll try to return whatever string we have before failing
		 * outright.
		 */
		err = vhd_parent_locator_get(vhd, &image->parent);
		if (!err)
			return 0;
	}

	loc = vhd_util_scan_get_parent_locator(vhd);
	if (!loc)
		return -EINVAL;

	return vhd_parent_locator_read(vhd, loc, &image->parent);
}

static int
vhd_util_scan_get_size(vhd_context_t *vhd, struct vhd_image *image)
{
	if (flags & VHD_SCAN_VOLUME) {
		image->size = image->target->size;
		return 0;
	}

	image->size = lseek64(vhd->fd, 0, SEEK_END);
	if (image->size == (off64_t)-1)
		return -errno;

	return 0;
}

static int
vhd_util_scan_open_file(vhd_context_t *vhd, struct vhd_image *image)
{
	int err, vhd_flags;

	vhd_flags = VHD_OPEN_RDONLY;
	if (flags & VHD_SCAN_FAST)
		vhd_flags |= VHD_OPEN_FAST;

	err = vhd_open(vhd, image->name, vhd_flags);
	if (err) {
		vhd->file      = NULL;
		image->message = "opening file";
		image->error   = err;
		return image->error;
	}

	return 0;
}

static int
vhd_util_scan_read_volume_headers(vhd_context_t *vhd, struct vhd_image *image)
{
	int err;
	char *buf;
	size_t size;
	struct target *target;

	buf    = NULL;
	target = image->target;
	size   = sizeof(vhd_footer_t) + sizeof(vhd_header_t);

	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err) {
		buf            = NULL;
		image->message = "allocating image";
		image->error   = -err;
		goto out;
	}

	err = vhd_seek(vhd, target->start, SEEK_SET);
	if (err) {
		image->message = "seeking to headers";
		image->error   = err;
		goto out;
	}

	err = vhd_read(vhd, buf, size);
	if (err) {
		image->message = "reading headers";
		image->error   = err;
		goto out;
	}

	memcpy(&vhd->footer, buf, sizeof(vhd_footer_t));
	vhd_footer_in(&vhd->footer);
	err = vhd_validate_footer(&vhd->footer);
	if (err) {
		image->message = "invalid footer";
		image->error   = err;
		goto out;
	}

	/* lvhd vhds should always be dynamic */
	if (vhd_type_dynamic(vhd)) {
		if (vhd->footer.data_offset != sizeof(vhd_footer_t))
			err = vhd_read_header_at(vhd, &vhd->header,
						 vhd->footer.data_offset +
						 target->start);
		else {
			memcpy(&vhd->header,
			       buf + sizeof(vhd_footer_t),
			       sizeof(vhd_header_t));
			vhd_header_in(&vhd->header);
			err = vhd_validate_header(&vhd->header);
		}

		if (err) {
			image->message = "reading header";
			image->error   = err;
			goto out;
		}

		vhd->spb = vhd->header.block_size >> VHD_SECTOR_SHIFT;
		vhd->bm_secs = secs_round_up_no_zero(vhd->spb >> 3);
	}

out:
	free(buf);
	return image->error;
}

static int
vhd_util_scan_open_volume(vhd_context_t *vhd, struct vhd_image *image)
{
	int err;
	struct target *target;

	target = image->target;
	memset(vhd, 0, sizeof(*vhd));
	vhd->oflags = VHD_OPEN_RDONLY | VHD_OPEN_FAST;

	if (target->end - target->start < 4096) {
		image->message = "device too small";
		image->error   = -EINVAL;
		return image->error;
	}

	vhd->file = strdup(image->name);
	if (!vhd->file) {
		image->message = "allocating device";
		image->error   = -ENOMEM;
		return image->error;
	}

	vhd->fd = open(target->device, O_RDONLY | O_DIRECT | O_LARGEFILE);
	if (vhd->fd == -1) {
		free(vhd->file);
		vhd->file = NULL;

		image->message = "opening device";
		image->error   = -errno;
		return image->error;
	}

	err = vhd_util_scan_read_volume_headers(vhd, image);
	if (err)
		return err;

	return 0;
}

static int
vhd_util_scan_open(vhd_context_t *vhd, struct vhd_image *image)
{
	struct target *target;

	target = image->target;

	if ((flags & VHD_SCAN_VOLUME) || !(flags & VHD_SCAN_PRETTY))
		image->name = target->name;
	else {
		image->name = realpath(target->name, NULL);
		if (!image->name) {
			image->name    = target->name;
			image->message = "resolving name";
			image->error   = -errno;
			return image->error;
		}
	}

	if (flags & VHD_SCAN_VOLUME)
		return vhd_util_scan_open_volume(vhd, image);
	else
		return vhd_util_scan_open_file(vhd, image);
}

static int
vhd_util_scan_targets(int cnt, struct target *targets)
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

		image.target = targets + i;

		err = vhd_util_scan_open(&vhd, &image);
		if (err) {
			ret = -EAGAIN;
			goto end;
		}

		image.capacity = vhd.footer.curr_size;

		err = vhd_util_scan_get_size(&vhd, &image);
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
			err = vhd_util_scan_get_parent(&vhd, &image);
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
		if (image.name != targets[i].name)
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

static inline int
copy_name(char *dst, const char *src)
{
	if (snprintf(dst, VHD_MAX_NAME_LEN, "%s", src) < VHD_MAX_NAME_LEN)
		return 0;

	return -ENAMETOOLONG;
}

static int
vhd_util_scan_init_file_target(struct target *target, const char *file)
{
	int err;
	struct stat stats;

	err = stat(file, &stats);
	if (err == -1)
		return -errno;

	err = copy_name(target->name, file);
	if (err)
		return err;

	err = copy_name(target->device, file);
	if (err)
		return err;

	target->start = 0;
	target->size  = stats.st_size;
	target->end   = stats.st_size;

	return 0;
}

static int
vhd_util_scan_find_file_targets(int cnt, char **names,
				const char *filter,
				struct target **_targets, int *_total)
{
	glob_t g;
	struct target *targets;
	int i, globs, err, total;

	total     = cnt;
	globs     = 0;
	*_total   = 0;
	*_targets = NULL;
	
	memset(&g, 0, sizeof(g));

	if (filter) {
		int gflags = ((flags & VHD_SCAN_FAST) ? GLOB_NOSORT : 0);

		errno = 0;
		err   = glob(filter, gflags, vhd_util_scan_error, &g);

		switch (err) {
		case GLOB_NOSPACE:
			err = -ENOMEM;
			break;
		case GLOB_ABORTED:
			err = -EIO;
			break;
		case GLOB_NOMATCH:
			err = -errno;
			break;
		}

		if (err) {
			vhd_util_scan_error(filter, err);
			return err;
		}

		globs  = g.gl_pathc;
		total += globs;
	}

	targets = calloc(total, sizeof(struct target));
	if (!targets) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < g.gl_pathc; i++) {
		err = vhd_util_scan_init_file_target(targets + i,
						     g.gl_pathv[i]);
		if (err) {
			vhd_util_scan_error(g.gl_pathv[i], err);
			if (!(flags & VHD_SCAN_NOFAIL))
				goto out;
		}
	}

	for (i = 0; i + globs < total; i++) {
		err = vhd_util_scan_init_file_target(targets + i + globs,
						     names[i]);
		if (err) {
			vhd_util_scan_error(names[i], err);
			if (!(flags & VHD_SCAN_NOFAIL))
				goto out;
		}
	}

	err       = 0;
	*_total   = total;
	*_targets = targets;

out:
	if (err)
		free(targets);
	if (filter)
		globfree(&g);

	return err;
}

static inline void
swap_volume(struct lv *lvs, int dst, int src)
{
	struct lv copy, *ldst, *lsrc;

	if (dst == src)
		return;

	lsrc = lvs + src;
	ldst = lvs + dst;

	memcpy(&copy, ldst, sizeof(copy));
	memcpy(ldst, lsrc, sizeof(*ldst));
	memcpy(lsrc, &copy, sizeof(copy));
}

static int
vhd_util_scan_sort_volumes(struct lv *lvs, int cnt,
			   const char *filter, int *_matches)
{
	struct lv *lv;
	int i, err, matches;

	matches   = 0;
	*_matches = 0;

	if (!filter)
		return 0;

	for (i = 0; i < cnt; i++) {
		lv  = lvs + i;

		err = fnmatch(filter, lv->name, FNM_PATHNAME);
		if (err) {
			if (err != FNM_NOMATCH) {
				vhd_util_scan_error(lv->name, err);
				if (!(flags & VHD_SCAN_NOFAIL))
					return err;
			}

			continue;
		}

		swap_volume(lvs, matches++, i);
	}

	*_matches = matches;
	return 0;
}

static int
vhd_util_scan_init_volume_target(struct target *target, struct lv *lv)
{
	int err;

	if (lv->first_segment.type != LVM_SEG_TYPE_LINEAR)
		return -ENOSYS;

	err = copy_name(target->name, lv->name);
	if (err)
		return err;

	err = copy_name(target->device, lv->first_segment.device);
	if (err)
		return err;

	target->size  = lv->size;
	target->start = lv->first_segment.pe_start;
	target->end   = target->start + lv->first_segment.pe_size;

	return 0;
}

static int
vhd_util_scan_find_volume_targets(int cnt, char **names,
				  const char *volume, const char *filter,
				  struct target **_targets, int *_total)
{
	struct vg vg;
	struct target *targets;
	int i, err, total, matches;

	*_total   = 0;
	*_targets = NULL;
	targets   = NULL;

	err = lvm_scan_vg(volume, &vg);
	if (err)
		return err;

	err = vhd_util_scan_sort_volumes(vg.lvs, vg.lv_cnt,
					 filter, &matches);
	if (err)
		goto out;

	total = matches;
	for (i = 0; i < cnt; i++) {
		err = vhd_util_scan_sort_volumes(vg.lvs + total,
						 vg.lv_cnt - total,
						 names[i], &matches);
		if (err)
			goto out;

		total += matches;
	}

	targets = calloc(total, sizeof(struct target));
	if (!targets) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < total; i++) {
		err = vhd_util_scan_init_volume_target(targets + i,
						       vg.lvs + i);
		if (err) {
			vhd_util_scan_error(vg.lvs[i].name, err);
			if (!(flags & VHD_SCAN_NOFAIL))
				goto out;
		}
	}

	err       = 0;
	*_total   = total;
	*_targets = targets;

out:
	if (err)
		free(targets);
	lvm_free_vg(&vg);
	return err;
}

static int
vhd_util_scan_find_targets(int cnt, char **names,
			   const char *volume, const char *filter,
			   struct target **targets, int *total)
{
	if (flags & VHD_SCAN_VOLUME)
		return vhd_util_scan_find_volume_targets(cnt, names,
							 volume, filter,
							 targets, total);
	return vhd_util_scan_find_file_targets(cnt, names,
					       filter, targets, total);
}

int
vhd_util_scan(int argc, char **argv)
{
	int c, ret, err, cnt;
	char *filter, *volume;
	struct target *targets;

	cnt     = 0;
	ret     = 0;
	err     = 0;
	flags   = 0;
	filter  = NULL;
	volume  = NULL;
	targets = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "m:fcl:pvh")) != -1) {
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
			volume = optarg;
			flags |= VHD_SCAN_VOLUME;
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

	if (!filter && argc - optind == 0) {
		err = -EINVAL;
		goto usage;
	}

	if (flags & VHD_SCAN_PRETTY)
		flags &= ~VHD_SCAN_FAST;

	err = vhd_util_scan_find_targets(argc - optind, argv + optind,
					 volume, filter, &targets, &cnt);
	if (err) {
		printf("scan failed: %d\n", err);
		return err;
	}

	if (!cnt) {
		printf("found no matches\n");
		return -ENOENT;
	}

	if (flags & VHD_SCAN_PRETTY) {
		err = vhd_util_scan_pretty_allocate_list(cnt);
		if (err) {
			printf("scan failed: no memory\n");
			return -ENOMEM;
		}
	}

	err = vhd_util_scan_targets(cnt, targets);

	if (flags & VHD_SCAN_PRETTY)
		vhd_util_scan_pretty_free_list();

	return ((flags & VHD_SCAN_NOFAIL) ? 0 : err);

usage:
	printf("usage: [OPTIONS] FILES\n"
	       "options: [-m match filter] [-f fast] [-c continue on failure] "
	       "[-p pretty print] [-v verbose] [-h help]\n");
	return err;
}
