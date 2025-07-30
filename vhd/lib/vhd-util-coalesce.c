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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

#include "libvhd.h"
#include "canonpath.h"

static int
__raw_io_write(int fd, char* buf, uint64_t sec, uint32_t secs)
{
	off64_t off;
	ssize_t ret;

	errno = 0;
	off = lseek64(fd, (off64_t)vhd_sectors_to_bytes(sec), SEEK_SET);
	if (off == (off64_t)-1) {
		printf("raw parent: seek(0x%08"PRIx64") failed: %d\n",
		       vhd_sectors_to_bytes(sec), -errno);
		return -errno;
	}

	ret = write(fd, buf, vhd_sectors_to_bytes(secs));
	if (ret == vhd_sectors_to_bytes(secs))
		return 0;

	printf("raw parent: write of 0x%"PRIx64" returned %zd, errno: %d\n",
	       vhd_sectors_to_bytes(secs), ret, -errno);
	return (errno ? -errno : -EIO);
}

/**
 * Coalesce a VHD allocation block
 *
 * @param[in] vhd the VHD being coalesced
 * @param[in] parent the VHD to coalesce to unless raw
 * @param[in] parent_fd raw FD to coalesce to unless VHD parent
 * @param[in] parent block the allocation block number to coalese
 * @return the number of sectors coalesced or negative errno on failure
 */
static int64_t
vhd_util_coalesce_block(vhd_context_t *vhd, vhd_context_t *parent,
			int parent_fd, uint64_t block)
{
	int err;
	uint32_t i;
	int64_t coalesced_size = 0;
	char *buf;
	char *map;
	uint64_t sec, secs;

	buf = NULL;
	map = NULL;
	sec = block * vhd->spb;

	if (vhd->bat.bat[block] == DD_BLK_UNUSED)
		return 0;

	if (vhd_has_batmap(vhd) && vhd_batmap_test(vhd, &vhd->batmap, block)) {
		err = vhd_read_block(vhd, block, &buf);
		if (err)
			goto done;

		if (parent->file)
			err = vhd_io_write(parent, buf, sec, vhd->spb);
		else
			err = __raw_io_write(parent_fd, buf, sec, vhd->spb);

		if (err == 0)
			coalesced_size = vhd->spb;

		goto done;
	}

	err = vhd_read_bitmap(vhd, block, &map);
	if (err)
		goto done;

	err = posix_memalign((void *)&buf, 4096, vhd->header.block_size);
	if (err) {
		err = -err;
		goto done;
	}

	for (i = 0; i < vhd->spb; i++) {
		if (!vhd_bitmap_test(vhd, map, i))
			continue;

		for (secs = 0; i + secs < vhd->spb; secs++)
			if (!vhd_bitmap_test(vhd, map, i + secs))
				break;

		err = vhd_read_at(vhd, block, i, vhd_sectors_to_bytes(secs),
				  buf + vhd_sectors_to_bytes(i));
		if (err)
			goto done;

		if (parent->file)
			err = vhd_io_write(parent,
					   buf + vhd_sectors_to_bytes(i),
					   sec + i, secs);
		else
			err = __raw_io_write(parent_fd,
					     buf + vhd_sectors_to_bytes(i),
					     sec + i, secs);
		if (err)
			goto done;

		coalesced_size += (int64_t)secs;
		i += (uint32_t)secs;
	}

	err = 0;

done:
	free(buf);
	free(map);
	if (err < 0)
		return err;

	return coalesced_size;
}

/**
 * Coalesce VHD to its immediate parent
 *
 * @param[in] from the VHD to coalesce
 * @param[in] to the VHD to coalesce to or NULL if raw
 * @param[in] to_fd the raws file to coalesce to or NULL if to is to be used
 * @param[in] progess whether to report progress as the operation is being performed
 * @return positive number of sectors coalesced or negative errno in the case of failure
 */
static int64_t
vhd_util_coalesce_onto(vhd_context_t *from,
		       vhd_context_t *to, int to_fd, int progress)
{
	int i;
	int64_t err;
	int64_t coalesced_size = 0;

	err = vhd_get_bat(from);
	if (err)
		goto out;

	if (vhd_has_batmap(from)) {
		err = vhd_get_batmap(from);
		if (err)
			goto out;
	}

	for (i = 0; i < from->bat.entries; i++) {
		if (progress) {
			printf("\r%6.2f%%",
			       ((float)i / (float)from->bat.entries) * 100.00);
			fflush(stdout);
		}
		err = vhd_util_coalesce_block(from, to, to_fd, (uint64_t)i);
		if (err < 0)
			goto out;

		coalesced_size += err;
	}

	err = 0;

	if (progress)
		printf("\r100.00%%\n");

out:
	if (err < 0)
		return err;

	return coalesced_size;
}

/**
 * Coalesce the VHD to its immediate parent
 *
 * @param[in] name the name (path) of the VHD to coalesce
 * @param[in] sparse whether the parent VHD should be written sparsely
 * @param[in] progess whether to report progress as the operation is being performed
 * @return positive number of sectors coalesced or negative errno in the case of failure
 */
static int64_t
vhd_util_coalesce_parent(const char *name, int sparse, int progress)
{
	char *pname;
	int64_t err;
	int parent_fd;
	vhd_context_t vhd, parent;

	parent_fd   = -1;
	parent.file = NULL;

	err = vhd_open(&vhd, name, VHD_OPEN_RDONLY);
	if (err) {
		printf("error opening %s: %" PRId64 "\n", name, err);
		return err;
	}

	if (vhd.footer.type != HD_TYPE_DIFF) {
		printf("coalescing of non-differencing disks is not supported\n");
		vhd_close(&vhd);
		return -EINVAL;
	}

	err = vhd_parent_locator_get(&vhd, &pname);
	if (err) {
		printf("error finding %s parent: %" PRId64 "\n", name, err);
		vhd_close(&vhd);
		return err;
	}

	if (vhd_parent_raw(&vhd)) {
		parent_fd = open_optional_odirect(pname, O_RDWR | O_DIRECT | O_LARGEFILE, 0644);
		if (parent_fd == -1) {
			err = -errno;
			printf("failed to open parent %s: %" PRId64 "\n", pname, err);
			free(pname);
			vhd_close(&vhd);
			return err;
		}
	} else {
		int flags = (sparse ? VHD_OPEN_IO_WRITE_SPARSE : 0);
		if (sparse) printf("opening for sparse writes\n");
		err = vhd_open(&parent, pname, VHD_OPEN_RDWR | flags);
		if (err) {
			printf("error opening %s: %" PRId64 "\n", pname, err);
			free(pname);
			vhd_close(&vhd);
			return err;
		}
	}

	err = vhd_util_coalesce_onto(&vhd, &parent, parent_fd, progress);

	free(pname);
	vhd_close(&vhd);
	if (parent.file)
		vhd_close(&parent);
	else
		close(parent_fd);
	return err;
}

int
vhd_util_coalesce(int argc, char **argv)
{
	char *name;
	int c, progress, sparse;
	int64_t result;

	name        = NULL;
	sparse      = 0;
	progress    = 0;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:o:a:x:sph")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 's':
			sparse = 1;
			break;
		case 'p':
			progress = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || optind != argc)
		goto usage;

	result = vhd_util_coalesce_parent(name, sparse, progress);

	if (result < 0) {
		/* -ve errors will be in range for int */
		printf("error coalescing: %d\n", (int)result);
		return result;
	}

	printf("Coalesced %" PRId64 " sectors", result);
	return 0;

usage:
	printf("options: <-n name> "
	       "[-s sparse] [-p progress] "
	       "[-h help]\n");
	return -EINVAL;
}
