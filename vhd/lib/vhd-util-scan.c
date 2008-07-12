/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>

#include "libvhd.h"

struct vhd_image {
	char                *name;
	char                *parent;
	uint64_t             capacity;
	off64_t              size;
	uint8_t              hidden;
};

static void
vhd_util_scan_print_image(struct vhd_image *image)
{
	printf("vhd=%s capacity=%llu size=%llu hidden=%u parent=%s\n",
	       image->name, image->capacity, image->size, image->hidden,
	       (image->parent ? : "none"));
}

static int
vhd_fs_filter(const struct dirent *ent)
{
	char *tmp;
	int len, elen;

	elen = strlen(".vhd");
	len  = strnlen(ent->d_name, sizeof(ent->d_name));
	tmp  = (char *)ent->d_name + len - elen;

	return (!strncmp(tmp, ".vhd", elen));
}

static int
vhd_vg_filter(const struct dirent *ent)
{
	char *tmp;

	tmp = strstr(ent->d_name, "VHD-");
	if (tmp != ent->d_name)
		return 0;

	return 1;
}

static int
vhd_util_scan_get_parent(const char *dir, vhd_context_t *vhd, char **parent)
{
	int i, err;
	vhd_parent_locator_t *loc;

	loc     = NULL;
	*parent = NULL;

	/*
	 * vhd_parent_locator_get checks for the existence of the parent file.
	 * if this call succeeds, all is well; if not, we'll try to return
	 * whatever string we have before failing outright.
	 */
	err = vhd_parent_locator_get(vhd, parent);
	if (!err)
		return 0;

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

	err = vhd_parent_locator_read(vhd, loc, parent);
	if (err) {
		printf("error getting parent of %s: %d\n", vhd->file, err);
		return err;
	}

	return 0;
}

static int
vhd_util_scan_directory(const char *target,
			int (*filter)(const struct dirent *))
{
	char *dir;
	int err, i, n;
	struct stat stats;
	vhd_context_t vhd;
	struct dirent **files;
	struct vhd_image image;

	err = stat(target, &stats);
	if (err) {
		printf("error accessing %s: %d\n", target, -errno);
		return -errno;
	}

	if (!S_ISDIR(stats.st_mode)) {
		printf("%s is not a directory\n", target);
		return -EINVAL;
	}

	dir = strdup(target);
	if (!dir) {
		printf("error allocating string\n");
		return -ENOMEM;
	}

	if (dir[strlen(dir) - 1] == '/')
		dir[strlen(dir) - 1] = '\0';

	n = scandir(target, &files, filter, alphasort);
	if (n < 0) {
		printf("scandir of %s failed: %d\n", target, -errno);
		free(dir);
		return -errno;
	}

	err = 0;
	for (i = 0; i < n; i++) {
		memset(&image, 0, sizeof(image));

		err = asprintf(&image.name, "%s/%s", dir, files[i]->d_name);
		if (err == -1) {
			printf("error allocating string for %s: %d\n",
			       files[i]->d_name, -errno);
			err = -errno;
			break;
		}

		err = vhd_open(&vhd, image.name, VHD_OPEN_RDONLY);
		if (err) {
			printf("error opening %s %d\n", image.name, err);
			free(image.name);
			break;
		}

		image.hidden   = vhd.footer.hidden;
		image.capacity = vhd.footer.curr_size;

		err = vhd_get_phys_size(&vhd, &image.size);
		if (err) {
			printf("error getting physical size of %s: %d\n",
			       image.name, err);
			goto next;
		}

		if (vhd.footer.type == HD_TYPE_DIFF) {
			err = vhd_util_scan_get_parent(dir,
						       &vhd, &image.parent);
			if (err) {
				printf("error reading parent of %s: %d\n",
				       image.name, err);
				goto next;
			}
		}

		vhd_util_scan_print_image(&image);

	next:
		free(image.name);
		free(image.parent);
		free(files[i]);
		files[i] = NULL;
		vhd_close(&vhd);
		if (err)
			break;
	}

	while (i < n)
		free(files[i++]);
	free(files);
	free(dir);

	return err;
}

static int
vhd_util_scan_files(const char *dir)
{
	return vhd_util_scan_directory(dir, vhd_fs_filter);
}

/*
 * assumes all volumes in volume group are are active
 */
static int
vhd_util_scan_volumes(const char *dir)
{
	return vhd_util_scan_directory(dir, vhd_vg_filter);
}

int
vhd_util_scan(int argc, char **argv)
{
	int c, err;
	char *name, *type;

	err  = 0;
	name = NULL;
	type = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "n:t:h")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 't':
			type = optarg;
			break;
		case 'h':
			goto usage;
		default:
			err = -EINVAL;
			goto usage;
		}
	}

	if (!name || !type)
		goto usage;

	if (!strcmp(type, "nfs") || !strcmp(type, "ext"))
		err = vhd_util_scan_files(name);
	else if (!strcmp(type, "lvhd"))
		err = vhd_util_scan_volumes(name);
	else {
		err = -EINVAL;
		goto usage;
	}

	return err;

usage:
	printf("options: -n name -t { nfs | ext | lvhd }\n");
	return err;
}
