/*
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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
	const char *name;

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
