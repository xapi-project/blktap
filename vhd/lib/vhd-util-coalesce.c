/*
 * Copyright (c) 2007, XenSource Inc.
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"

static int
__raw_io_write(int fd, char* buf, uint64_t sec, uint32_t secs)
{
	off64_t off;
	size_t ret;

	errno = 0;
	off = lseek64(fd, vhd_sectors_to_bytes(sec), SEEK_SET);
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

/*
 * Use 'parent' if the parent is VHD, and 'parent_fd' if the parent is raw
 */
static int
vhd_util_coalesce_block(vhd_context_t *vhd, vhd_context_t *parent,
			int parent_fd, uint64_t block)
{
	int i, err;
	char *buf, *map;
	uint64_t sec, secs;

	buf = NULL;
	map = NULL;
	sec = block * vhd->spb;

	if (vhd->bat.bat[block] == DD_BLK_UNUSED)
		return 0;

	err = posix_memalign((void **)&buf, 4096, vhd->header.block_size);
	if (err)
		return -err;

	err = vhd_io_read(vhd, buf, sec, vhd->spb);
	if (err)
		goto done;

	if (vhd_has_batmap(vhd) && vhd_batmap_test(vhd, &vhd->batmap, block)) {
		if (parent->file)
			err = vhd_io_write(parent, buf, sec, vhd->spb);
		else
			err = __raw_io_write(parent_fd, buf, sec, vhd->spb);
		goto done;
	}

	err = vhd_read_bitmap(vhd, block, &map);
	if (err)
		goto done;

	for (i = 0; i < vhd->spb; i++) {
		if (!vhd_bitmap_test(vhd, map, i))
			continue;

		for (secs = 0; i + secs < vhd->spb; secs++)
			if (!vhd_bitmap_test(vhd, map, i + secs))
				break;

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

		i += secs;
	}

	err = 0;

done:
	free(buf);
	free(map);
	return err;
}

static int
vhd_util_coalesce_onto(vhd_context_t *from,
		       vhd_context_t *to, int to_fd, int progress)
{
	int err;
	uint64_t i;

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
		err = vhd_util_coalesce_block(from, to, to_fd, i);
		if (err)
			goto out;
	}

	err = 0;

	if (progress)
		printf("\r100.00%%\n");

out:
	return err;
}

static int
vhd_util_coalesce_parent(const char *name, int sparse, int progress)
{
	char *pname;
	int err, parent_fd;
	vhd_context_t vhd, parent;

	parent_fd   = -1;
	parent.file = NULL;

	err = vhd_open(&vhd, name, VHD_OPEN_RDONLY);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	err = vhd_parent_locator_get(&vhd, &pname);
	if (err) {
		printf("error finding %s parent: %d\n", name, err);
		vhd_close(&vhd);
		return err;
	}

	if (vhd_parent_raw(&vhd)) {
		parent_fd = open(pname, O_RDWR | O_DIRECT | O_LARGEFILE, 0644);
		if (parent_fd == -1) {
			err = -errno;
			printf("failed to open parent %s: %d\n", pname, err);
			vhd_close(&vhd);
			return err;
		}
	} else {
		int flags = (sparse ? VHD_OPEN_IO_WRITE_SPARSE : 0);
		if (sparse) printf("opening for sparse writes\n");
		err = vhd_open(&parent, pname, VHD_OPEN_RDWR | flags);
		if (err) {
			printf("error opening %s: %d\n", pname, err);
			free(pname);
			vhd_close(&vhd);
			return err;
		}
	}

	err = vhd_util_coalesce_onto(&vhd, &parent, parent_fd, progress);

 done:
	free(pname);
	vhd_close(&vhd);
	if (parent.file)
		vhd_close(&parent);
	else
		close(parent_fd);
	return err;
}

struct vhd_list_entry {
	int                raw;
	int                raw_fd;
	vhd_context_t      vhd;
	struct list_head   next;
};

static int
vhd_util_pathcmp(const char *a, const char *b, int *cmp)
{
	int err;
	char *apath = NULL, *bpath = NULL;

	apath = realpath(a, NULL);
	if (!apath) {
		err = -errno;
		goto out;
	}

	bpath = realpath(b, NULL);
	if (!bpath) {
		err = -errno;
		goto out;
	}

	*cmp = strcmp(apath, bpath);
	err  = 0;

out:
	free(apath);
	free(bpath);
	return err;
}

static void
vhd_util_coalesce_free_chain(struct list_head *head)
{
	struct vhd_list_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, head, next) {
		if (entry->raw)
			close(entry->raw_fd);
		else
			vhd_close(&entry->vhd);
		list_del(&entry->next);
		free(entry);
	}

	INIT_LIST_HEAD(head);
}

static int
vhd_util_coalesce_load_chain(struct list_head *head,
			     const char *cname, const char *aname, int sparse)
{
	char *next;
	vhd_context_t *child;
	int err, cmp, vhd_flags;
	struct vhd_list_entry *entry;

	next  = NULL;
	entry = NULL;
	INIT_LIST_HEAD(head);

	vhd_flags = VHD_OPEN_RDWR | (sparse ? VHD_OPEN_IO_WRITE_SPARSE : 0);

	err = vhd_util_pathcmp(cname, aname, &cmp);
	if (err)
		goto out;

	if (!cmp) {
		err = -EINVAL;
		goto out;
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry)
		goto out;

	err = vhd_open(&entry->vhd, cname, vhd_flags);
	if (err)
		goto out;

	err = vhd_get_bat(&entry->vhd);
	if (err)
		goto out;

	if (vhd_has_batmap(&entry->vhd)) {
		err = vhd_get_batmap(&entry->vhd);
		if (err)
			goto out;
	}

	child = &entry->vhd;
	list_add(&entry->next, head);

	for (;;) {
		int raw;

		if (entry->raw || entry->vhd.footer.type != HD_TYPE_DIFF) {
			err = -ENOENT;
			goto out;
		}

		if (child->header.block_size != entry->vhd.header.block_size) {
			err = -EINVAL;
			goto out;
		}

		err = vhd_parent_locator_get(&entry->vhd, &next);
		if (err)
			goto out;

		raw = vhd_parent_raw(&entry->vhd);

		entry = calloc(1, sizeof(*entry));
		if (!entry)
			goto out;

		if (raw) {
			entry->raw = raw;
			entry->raw_fd = open(next,
					     O_RDWR | O_DIRECT | O_LARGEFILE);
			if (entry->raw_fd == -1) {
				err = -errno;
				goto out;
			}
		} else {
			err = vhd_open(&entry->vhd, next, vhd_flags);
			if (err)
				goto out;

			err = vhd_get_bat(&entry->vhd);
			if (err)
				goto out;

			if (vhd_has_batmap(&entry->vhd)) {
				err = vhd_get_batmap(&entry->vhd);
				if (err)
					goto out;
			}
		}

		list_add_tail(&entry->next, head);

		err = vhd_util_pathcmp(next, aname, &cmp);
		if (err)
			goto out;

		if (!cmp)
			goto done;

		free(next);
		next = NULL;
	}

done:
	err = 0;
out:
	if (err) {
		if (entry && list_empty(&entry->next)) {
			if (entry->vhd.file)
				vhd_close(&entry->vhd);
			else if (entry->raw)
				close(entry->raw_fd);
			free(entry);
		}
		vhd_util_coalesce_free_chain(head);
	}
	return err;
}

static int
vhd_util_coalesce_clear_bitmap(vhd_context_t *child, char *cmap,
			       vhd_context_t *ancestor, const uint64_t block)
{
	char *amap = NULL;
	int i, dirty, err;

	if (child->spb != ancestor->spb) {
		err = -EINVAL;
		goto out;
	}

	if (block >= ancestor->bat.entries)
		goto done;

	if (ancestor->bat.bat[block] == DD_BLK_UNUSED)
		goto done;

	err = vhd_read_bitmap(ancestor, block, &amap);
	if (err)
		goto out;

	for (i = 0; i < child->spb; i++) {
		if (vhd_bitmap_test(child, cmap, i)) {
			if (vhd_bitmap_test(ancestor, amap, i)) {
				dirty = 1;
				vhd_bitmap_clear(ancestor, amap, i);
			}
		}
	}

	if (dirty) {
		err = vhd_write_bitmap(ancestor, block, amap);
		if (err)
			goto out;
		if (vhd_has_batmap(ancestor) &&
		    vhd_batmap_test(ancestor, &ancestor->batmap, block)) {
			vhd_batmap_clear(ancestor, &ancestor->batmap, block);
			err = vhd_write_batmap(ancestor, &ancestor->batmap);
			if (err)
				goto out;
		}
	}

done:
	err = 0;
out:
	free(amap);
	return err;
}

static int
vhd_util_coalesce_clear_bitmaps(struct list_head *chain, vhd_context_t *child,
				vhd_context_t *ancestor, uint64_t block)
{
	int err;
	char *map = NULL;
	struct vhd_list_entry *entry;

	if (child->bat.bat[block] == DD_BLK_UNUSED)
		goto done;

	err = vhd_read_bitmap(child, block, &map);
	if (err)
		goto out;

	list_for_each_entry(entry, chain, next) {
		if (&entry->vhd == child)
			continue;
		if (&entry->vhd == ancestor)
			break;
		err = vhd_util_coalesce_clear_bitmap(child, map,
						     &entry->vhd, block);
		if (err)
			goto out;
	}

done:
	err = 0;
out:
	free(map);
	return err;
}

static int
vhd_util_coalesce_ancestor(const char *cname,
			   const char *aname, int sparse, int progress)
{
	uint64_t i;
	int err, raw_fd;
	struct list_head chain;
	struct vhd_list_entry *entry;
	vhd_context_t *child, *ancestor;

	child    = NULL;
	ancestor = NULL;

	err = vhd_util_coalesce_load_chain(&chain, cname, aname, sparse);
	if (err)
		goto out;

	list_for_each_entry(entry, &chain, next) {
		if (!child)
			child = &entry->vhd;
		else if (list_is_last(&entry->next, &chain)) {
			ancestor = &entry->vhd;
			raw_fd = entry->raw_fd;
			break;
		}
	}

	if (!ancestor) {
		err = -EINVAL;
		goto out;
	}

	err = vhd_util_coalesce_onto(child, ancestor, raw_fd, progress);
	if (err)
		goto out;

	for (i = 0; i < child->bat.entries; i++) {
		err = vhd_util_coalesce_clear_bitmaps(&chain,
						      child, ancestor, i);
		if (err)
			goto out;
	}

out:
	vhd_util_coalesce_free_chain(&chain);
	return err;
}

static int
vhd_util_coalesce_open_output(vhd_context_t *dst,
			      vhd_context_t *src, const char *name, int flags)
{
	int err;

	err = access(name, F_OK);
	if (!err) {
		printf("%s already exists\n", name);
		return -EEXIST;
	} else if (errno != ENOENT) {
		printf("error checking %s: %d\n", name, errno);
		return -errno;
	}

	err = vhd_create(name, src->footer.curr_size, HD_TYPE_DYNAMIC, 0, 0);
	if (err) {
		printf("error creating %s: %d\n", name, err);
		return err;
	}

	err = vhd_open(dst, name, VHD_OPEN_RDWR | flags);
	if (err || dst->header.block_size != src->header.block_size) {
		printf("error opening %s: %d\n", name, (err ? : EINVAL));
		unlink(name);
		return err ? : EINVAL;
	}

	return 0;
}

/*
 * read block from @src chain and write it to @dst, unless it is all zeros
 */
static int
vhd_util_coalesce_block_out(vhd_context_t *dst,
			    vhd_context_t *src, uint64_t block)
{
	int i, err;
	uint64_t sec;
	char *buf, *p;

	buf = NULL;
	sec = block * src->spb;

	err = posix_memalign((void **)&buf, 4096, src->header.block_size);
	if (err)
		return -err;

	err = vhd_io_read(src, buf, sec, src->spb);
	if (err)
		goto done;

	for (p = buf, i = 0; i < src->header.block_size; i++, p++) {
		if (*p) {
			err = vhd_io_write(dst, buf, sec, src->spb);
			break;
		}
	}

done:
	free(buf);
	return err;
}

static int
vhd_util_coalesce_out(const char *src_name, const char *dst_name,
		      int sparse, int progress)
{
	uint64_t i;
	int err, flags;
	vhd_context_t src, dst;

	err = vhd_open(&src, src_name, VHD_OPEN_RDONLY | VHD_OPEN_CACHED);
	if (err)
		return err;

	flags = (sparse ? VHD_OPEN_IO_WRITE_SPARSE : 0);
	err = vhd_util_coalesce_open_output(&dst, &src, dst_name, flags);
	if (err) {
		vhd_close(&src);
		return err;
	}

	err = vhd_get_bat(&src);
	if (err)
		goto done;

	if (vhd_has_batmap(&src)) {
		err = vhd_get_batmap(&src);
		if (err)
			goto done;
	}

	for (i = 0; i < src.bat.entries; i++) {
		if (progress) {
			printf("\r%6.2f%%",
			       ((float)i / (float)src.bat.entries) * 100.0);
			fflush(stdout);
		}
		err = vhd_util_coalesce_block_out(&dst, &src, i);
		if (err)
			goto done;
	}

	err = 0;

	if (progress)
		printf("\r100.00%%\n");

done:
	if (err)
		unlink(dst.file);
	vhd_close(&src);
	vhd_close(&dst);
	return err;
}

int
vhd_util_coalesce(int argc, char **argv)
{
	uint64_t i;
	char *name, *oname, *ancestor;
	int err, c, progress, sparse;

	name      = NULL;
	oname     = NULL;
	ancestor  = NULL;
	sparse    = 0;
	progress  = 0;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:o:a:sph")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'o':
			oname = optarg;
			break;
		case 'a':
			ancestor = optarg;
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

	if (oname && ancestor)
		goto usage;

	if (oname)
		err = vhd_util_coalesce_out(name, oname, sparse, progress);
	else if (ancestor)
		err = vhd_util_coalesce_ancestor(name, ancestor,
						 sparse, progress);
	else
		err = vhd_util_coalesce_parent(name, sparse, progress);

	if (err)
		printf("error coalescing: %d\n", err);

	return err;

usage:
	printf("options: <-n name> [-a ancestor] "
	       "[-o output] [-s sparse] [-p progress] [-h help]\n");
	return -EINVAL;
}
