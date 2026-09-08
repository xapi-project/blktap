/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include "tapdisk-storage.h"

#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC 0x6969
#endif

static int
__tapdisk_fs_storage_type(const char *rpath)
{
	struct statfs fst;
	int type, err;

	err = statfs(rpath, &fst);
	if (err)
		return -errno;

	switch (fst.f_type) {
	case NFS_SUPER_MAGIC:
		type = TAPDISK_STORAGE_TYPE_NFS;
		break;
	default:
		type = TAPDISK_STORAGE_TYPE_EXT;
		break;
	}

	return type;
}

static int
__tapdisk_blk_storage_type(const char *rpath)
{
	return TAPDISK_STORAGE_TYPE_LVM;
}

int
tapdisk_storage_type(const char *path)
{
	char rpath[PATH_MAX], *p;
	struct stat st;
	int err, rv;

	p = realpath(path, rpath);
	if (!p)
		return -errno;

	err = stat(rpath, &st);
	if (err)
		return -errno;

	switch (st.st_mode & S_IFMT) {
	case S_IFBLK:
		rv = __tapdisk_blk_storage_type(rpath);
		break;
	case S_IFREG:
		rv = __tapdisk_fs_storage_type(rpath);
		break;
	default:
		rv = -EINVAL;
		break;
	}

	return rv;
}

const char *
tapdisk_storage_name(int type)
{
	switch (type) {
	case TAPDISK_STORAGE_TYPE_NFS:
		return "nfs";
	case TAPDISK_STORAGE_TYPE_EXT:
		return "ext";
	case TAPDISK_STORAGE_TYPE_LVM:
		return "lvm";
	case -1:
		return "n/a";
	default:
		return "<unknown-type>";
	}
}
