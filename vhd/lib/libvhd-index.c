/*
 * Copyright (c) 2008, XenSource Inc.
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

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "libvhd.h"
#include "libvhd-index.h"
#include "relative-path.h"

typedef struct vhdi_path                    vhdi_path_t;
typedef struct vhdi_header                  vhdi_header_t;
typedef struct vhdi_bat_header              vhdi_bat_header_t;
typedef struct vhdi_file_table_header       vhdi_file_table_header_t;
typedef struct vhdi_file_table_entry        vhdi_file_table_entry_t;

static const char                           VHDI_HEADER_COOKIE[] = "vhdindex";
static const char                           VHDI_BAT_HEADER_COOKIE[] = "vhdi-bat";
static const char                           VHDI_FILE_TABLE_HEADER_COOKIE[] = "vhdifile";

struct vhdi_path {
	char                                path[VHD_MAX_NAME_LEN];
	uint16_t                            bytes;
};

struct vhdi_header {
	char                                cookie[8];
	uint32_t                            vhd_block_size;
	uint64_t                            table_offset;
};

struct vhdi_bat_header {
	char                                cookie[8];
	uint64_t                            vhd_blocks;
	uint32_t                            vhd_block_size;
	vhdi_path_t                         vhd_path;
	vhdi_path_t                         index_path;
	vhdi_path_t                         file_table_path;
	uint64_t                            table_offset;
};

struct vhdi_file_table_header {
	char                                cookie[8];
	uint32_t                            files;
	uint64_t                            table_offset;
};

struct vhdi_file_table_entry {
	vhdi_path_t                         p;
	vhdi_file_id_t                      file_id;
	uuid_t                              vhd_uuid;
	uint32_t                            vhd_timestamp;
};

static inline int
vhdi_seek(vhdi_context_t *ctx, off64_t off, int whence)
{
	int err;

	err = lseek64(ctx->fd, off, whence);
	if (err == (off64_t)-1)
		return -errno;

	return 0;
}

static inline off64_t
vhdi_position(vhdi_context_t *ctx)
{
	return lseek64(ctx->fd, 0, SEEK_CUR);
}

static inline int
vhdi_read(vhdi_context_t *ctx, void *buf, size_t size)
{
	int err;

	err = read(ctx->fd, buf, size);
	if (err != size)
		return (errno ? -errno : -EIO);

	return 0;
}

static inline int
vhdi_write(vhdi_context_t *ctx, void *buf, size_t size)
{
	int err;

	err = write(ctx->fd, buf, size);
	if (err != size)
		return (errno ? -errno : -EIO);

	return 0;
}

static inline int
vhdi_check_block_size(uint32_t block_size)
{
	int i, cnt;

	cnt = 0;
	for (i = 0; i < 32; i++)
		if ((block_size >> i) & 0x0001)
			cnt++;

	if (cnt == 1)
		return 0;

	return -EINVAL;
}

static inline void
vhdi_header_in(vhdi_header_t *header)
{
	BE32_IN(&header->vhd_block_size);
	BE64_IN(&header->table_offset);
}

static inline void
vhdi_header_out(vhdi_header_t *header)
{
	BE32_OUT(&header->vhd_block_size);
	BE64_OUT(&header->table_offset);
}

static inline int
vhdi_header_validate(vhdi_header_t *header)
{
	if (memcmp(header->cookie, VHDI_HEADER_COOKIE, sizeof(header->cookie)))
		return -EINVAL;

	return vhdi_check_block_size(header->vhd_block_size);
}

void
vhdi_entry_in(vhdi_entry_t *entry)
{
	BE32_IN(&entry->file_id);
	BE32_IN(&entry->offset);
}

static inline vhdi_entry_t
vhdi_entry_out(vhdi_entry_t *entry)
{
	vhdi_entry_t e;

	e = *entry;
	BE32_OUT(&e.file_id);
	BE32_OUT(&e.offset);

	return e;
}

static inline void
vhdi_path_in(vhdi_path_t *path)
{
	BE16_IN(&path->bytes);
}

static inline void
vhdi_path_out(vhdi_path_t *path)
{
	BE16_OUT(&path->bytes);
}

static inline void
vhdi_bat_header_in(vhdi_bat_header_t *header)
{
	BE64_IN(&header->vhd_blocks);
	BE32_IN(&header->vhd_block_size);
	vhdi_path_in(&header->vhd_path);
	vhdi_path_in(&header->index_path);
	vhdi_path_in(&header->file_table_path);
	BE64_IN(&header->table_offset);
}

static inline void
vhdi_bat_header_out(vhdi_bat_header_t *header)
{
	BE64_OUT(&header->vhd_blocks);
	BE32_OUT(&header->vhd_block_size);
	vhdi_path_out(&header->vhd_path);
	vhdi_path_out(&header->index_path);
	vhdi_path_out(&header->file_table_path);
	BE64_OUT(&header->table_offset);
}

static inline int
vhdi_path_validate(vhdi_path_t *path)
{
	int i;

	if (path->bytes >= VHD_MAX_NAME_LEN - 1)
		return -ENAMETOOLONG;

	for (i = 0; i < path->bytes; i++)
		if (path->path[i] == '\0')
			return 0;

	return -EINVAL;
}

static inline char *
vhdi_path_expand(const char *src, vhdi_path_t *dest, int *err)
{
	int len;
	char *path, *base, *absolute_path, copy[VHD_MAX_NAME_LEN];

	strcpy(copy, src);
	base = dirname(copy);

	*err = asprintf(&path, "%s/%s", base, dest->path);
	if (*err == -1) {
		*err = -ENOMEM;
		return NULL;
	}

	absolute_path = realpath(path, NULL);
	free(path);

	if (!absolute_path) {
		*err = -errno;
		return NULL;
	}

	len = strnlen(absolute_path, VHD_MAX_NAME_LEN - 1);
	if (len == VHD_MAX_NAME_LEN - 1) {
		free(absolute_path);
		*err = -ENAMETOOLONG;
		return NULL;
	}

	*err = 0;
	return absolute_path;
}

static inline int
vhdi_bat_header_validate(vhdi_bat_header_t *header)
{
	int err;

	if (memcmp(header->cookie,
		   VHDI_BAT_HEADER_COOKIE, sizeof(header->cookie)))
		return -EINVAL;

	err = vhdi_check_block_size(header->vhd_block_size);
	if (err)
		return err;

	err = vhdi_path_validate(&header->vhd_path);
	if (err)
		return err;

	err = vhdi_path_validate(&header->index_path);
	if (err)
		return err;

	err = vhdi_path_validate(&header->file_table_path);
	if (err)
		return err;

	return 0;
}

static inline int
vhdi_bat_load_header(int fd, vhdi_bat_header_t *header)
{
	int err;

	err = lseek64(fd, 0, SEEK_SET);
	if (err == (off64_t)-1)
		return -errno;

	err = read(fd, header, sizeof(vhdi_bat_header_t));
	if (err != sizeof(vhdi_bat_header_t))
		return (errno ? -errno : -EIO);

	vhdi_bat_header_in(header);
	return vhdi_bat_header_validate(header);
}

static inline void
vhdi_file_table_header_in(vhdi_file_table_header_t *header)
{
	BE32_OUT(&header->files);
	BE64_OUT(&header->table_offset);
}

static inline void
vhdi_file_table_header_out(vhdi_file_table_header_t *header)
{
	BE32_OUT(&header->files);
	BE64_OUT(&header->table_offset);
}

static inline int
vhdi_file_table_header_validate(vhdi_file_table_header_t *header)
{
	if (memcmp(header->cookie,
		   VHDI_FILE_TABLE_HEADER_COOKIE, sizeof(header->cookie)))
		return -EINVAL;

	return 0;
}

static inline int
vhdi_file_table_load_header(int fd, vhdi_file_table_header_t *header)
{
	int err;

	err = lseek64(fd, 0, SEEK_SET);
	if (err == (off64_t)-1)
		return -errno;

	err = read(fd, header, sizeof(vhdi_file_table_header_t));
	if (err != sizeof(vhdi_file_table_header_t))
		return (errno ? -errno : -EIO);

	vhdi_file_table_header_in(header);
	return vhdi_file_table_header_validate(header);
}

static inline int
vhdi_file_table_write_header(int fd, vhdi_file_table_header_t *header)
{
	int err;

	err = lseek64(fd, 0, SEEK_SET);
	if (err == (off64_t)-1)
		return -errno;

	err = vhdi_file_table_header_validate(header);
	if (err)
		return err;

	vhdi_file_table_header_out(header);

	err = write(fd, header, sizeof(vhdi_file_table_header_t));
	if (err != sizeof(vhdi_file_table_header_t))
		return (errno ? -errno : -EIO);

	return 0;
}

static inline void
vhdi_file_table_entry_in(vhdi_file_table_entry_t *entry)
{
	vhdi_path_in(&entry->p);
	BE32_IN(&entry->file_id);
	BE32_IN(&entry->vhd_timestamp);
}

static inline void
vhdi_file_table_entry_out(vhdi_file_table_entry_t *entry)
{
	vhdi_path_out(&entry->p);
	BE32_OUT(&entry->file_id);
	BE32_OUT(&entry->vhd_timestamp);
}

static inline int
vhdi_file_table_entry_validate(vhdi_file_table_entry_t *entry)
{
	return vhdi_path_validate(&entry->p);
}

static inline int
vhdi_file_table_entry_validate_vhd(vhdi_file_table_entry_t *entry,
				   const char *path)
{
	int err;
	vhd_context_t vhd;
	struct stat stats;

	err = stat(path, &stats);
	if (err == -1)
		return -errno;

	if (entry->vhd_timestamp != vhd_time(stats.st_mtime))
		return -EINVAL;

	err = vhd_open(&vhd, path, VHD_OPEN_RDONLY);
	if (err)
		return err;

	err = vhd_get_footer(&vhd);
	if (err)
		goto out;

	if (uuid_compare(entry->vhd_uuid, vhd.footer.uuid)) {
		err = -EINVAL;
		goto out;
	}

out:
	vhd_close(&vhd);
	return err;
}

int
vhdi_create(const char *name, uint32_t vhd_block_size)
{
	char *buf;
	int err, fd;
	size_t size;
	vhdi_header_t header;

	memset(&header, 0, sizeof(vhdi_header_t));

	err = vhdi_check_block_size(vhd_block_size);
	if (err)
		return err;

	err = access(name, F_OK);
	if (!err || errno != ENOENT)
		return (err ? err : -EEXIST);

	memcpy(header.cookie, VHDI_HEADER_COOKIE, sizeof(header.cookie));
	header.vhd_block_size = vhd_block_size;
	header.table_offset   = vhd_bytes_padded(sizeof(vhdi_header_t));

	err = vhdi_header_validate(&header);
	if (err)
		return err;

	vhdi_header_out(&header);

	size = vhd_bytes_padded(sizeof(vhdi_header_t));
	err  = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err)
		return -err;

	memset(buf, 0, size);
	memcpy(buf, &header, sizeof(vhdi_header_t));

	fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd == -1)
		return -errno;

	err = write(fd, buf, size);
	if (err != size) {
		err = (errno ? -errno : -EIO);
		goto fail;
	}

	close(fd);
	free(buf);

	return 0;

fail:
	close(fd);
	free(buf);
	unlink(name);
	return err;
}

int
vhdi_open(vhdi_context_t *ctx, const char *file, int flags)
{
	int err, fd;
	size_t size;
	char *name, *buf;
	vhdi_header_t header;

	buf = NULL;
	memset(ctx, 0, sizeof(vhdi_context_t));

	name = strdup(file);
	if (!name)
		return -ENOMEM;

	fd = open(file, flags | O_LARGEFILE);
	if (fd == -1) {
		free(name);
		return -errno;
	}

	size = vhd_bytes_padded(sizeof(vhdi_header_t));
	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err) {
		err = -err;
		goto fail;
	}

	err = read(fd, buf, size);
	if (err != size) {
		err = (errno ? -errno : -EIO);
		goto fail;
	}

	memcpy(&header, buf, sizeof(vhdi_header_t));
	free(buf);
	buf = NULL;

	vhdi_header_in(&header);
	err = vhdi_header_validate(&header);
	if (err)
		goto fail;

	ctx->fd             = fd;
	ctx->name           = name;
	ctx->spb            = header.vhd_block_size >> VHD_SECTOR_SHIFT;
	ctx->vhd_block_size = header.vhd_block_size;

	return 0;

fail:
	close(fd);
	free(buf);
	free(name);
	return err;
}

void
vhdi_close(vhdi_context_t *ctx)
{
	close(ctx->fd);
	free(ctx->name);
}

int
vhdi_read_block(vhdi_context_t *ctx, vhdi_block_t *block, uint32_t sector)
{
	int i, err;
	size_t size;

	err = vhdi_seek(ctx, vhd_sectors_to_bytes(sector), SEEK_SET);
	if (err)
		return err;

	size = vhd_bytes_padded(ctx->spb * sizeof(vhdi_entry_t));

	block->entries = ctx->spb;
	err = posix_memalign((void **)&block->table, VHD_SECTOR_SIZE, size);
	if (err)
		return -err;

	err = vhdi_read(ctx, block->table, size);
	if (err)
		goto fail;

	for (i = 0; i < block->entries; i++)
		vhdi_entry_in(&block->table[i]);

	return 0;

fail:
	free(block->table);
	return err;
}

int
vhdi_write_block(vhdi_context_t *ctx, vhdi_block_t *block, uint32_t sector)
{
	char *buf;
	int i, err;
	size_t size;
	vhdi_entry_t *entries;

	err = vhdi_seek(ctx, vhd_sectors_to_bytes(sector), SEEK_SET);
	if (err)
		return err;

	size = vhd_bytes_padded(ctx->spb * sizeof(vhdi_entry_t));
	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err)
		return -err;

	memset(buf, 0, size);
	entries = (vhdi_entry_t *)buf;

	for (i = 0; i < block->entries; i++)
		entries[i] = vhdi_entry_out(&block->table[i]);

	err = vhdi_write(ctx, entries, size);
	if (err)
		goto out;

	err = 0;

out:
	free(entries);
	return err;
}

int
vhdi_append_block(vhdi_context_t *ctx, vhdi_block_t *block, uint32_t *sector)
{
	char *buf;
	int i, err;
	off64_t off;
	size_t size;
	vhdi_entry_t *entries;

	err = vhdi_seek(ctx, 0, SEEK_END);
	if (err)
		return err;

	off = vhd_bytes_padded(vhdi_position(ctx));

	err = vhdi_seek(ctx, off, SEEK_SET);
	if (err)
		return err;

	size = vhd_bytes_padded(block->entries * sizeof(vhdi_entry_t));
	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err)
		return -err;

	memset(buf, 0, size);
	entries = (vhdi_entry_t *)buf;

	for (i = 0; i < block->entries; i++)
		entries[i] = vhdi_entry_out(&block->table[i]);

	err = vhdi_write(ctx, entries, block->entries * sizeof(vhdi_entry_t));
	if (err)
		goto out;

	err     = 0;
	*sector = off >> VHD_SECTOR_SHIFT;
out:
	if (err) {
		int gcc = ftruncate(ctx->fd, off);
	}
	free(entries);
	return err;
}

static int
vhdi_copy_path_to(vhdi_path_t *path, const char *src, const char *dest)
{
	int len, err;
	char *file, *absolute_path, *relative_path, copy[VHD_MAX_NAME_LEN];

	strcpy(copy, dest);

	file          = basename(copy);
	absolute_path = realpath(copy, NULL);
	relative_path = NULL;

	if (!absolute_path || !strcmp(file, "")) {
		err = (errno ? -errno : -EINVAL);
		goto out;
	}

	relative_path = relative_path_to((char *)src, absolute_path, &err);
	if (!relative_path || err) {
		err = (err ? err : -EINVAL);
		goto out;
	}

	len = strnlen(relative_path, VHD_MAX_NAME_LEN - 1);
	if (len == VHD_MAX_NAME_LEN - 1) {
		err = -ENAMETOOLONG;
		goto out;
	}

	strcpy(path->path, relative_path);
	path->bytes = len + 1;

	err = 0;

out:
	free(absolute_path);
	free(relative_path);
	return err;
}

int
vhdi_bat_create(const char *name, const char *vhd,
		const char *index, const char *file_table)
{
	int err, fd;
	off64_t off;
	vhd_context_t ctx;
	vhdi_bat_header_t header;

	memset(&header, 0, sizeof(vhdi_bat_header_t));

	err = access(name, F_OK);
	if (!err || errno != ENOENT)
		return (err ? -err : -EEXIST);

	err = vhd_open(&ctx, vhd, VHD_OPEN_RDONLY);
	if (err)
		return err;

	err = vhd_get_header(&ctx);
	if (err) {
		vhd_close(&ctx);
		return err;
	}

	header.vhd_blocks     = ctx.header.max_bat_size;
	header.vhd_block_size = ctx.header.block_size;

	vhd_close(&ctx);

	fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd == -1)
		return -errno;

	err = vhdi_copy_path_to(&header.vhd_path, name, vhd);
	if (err)
		goto fail;

	err = vhdi_copy_path_to(&header.index_path, name, index);
	if (err)
		goto fail;

	err = vhdi_copy_path_to(&header.file_table_path, name, file_table);
	if (err)
		goto fail;

	off = vhd_bytes_padded(sizeof(vhdi_bat_header_t));

	header.table_offset = off;
	memcpy(header.cookie, VHDI_BAT_HEADER_COOKIE, sizeof(header.cookie));

	err = vhdi_bat_header_validate(&header);
	if (err)
		goto fail;

	vhdi_bat_header_out(&header);

	err = write(fd, &header, sizeof(vhdi_bat_header_t));
	if (err != sizeof(vhdi_bat_header_t)) {
		err = (errno ? -errno : -EIO);
		goto fail;
	}

	close(fd);
	return 0;

fail:
	close(fd);
	unlink(name);
	return err;
}

int
vhdi_bat_load(const char *name, vhdi_bat_t *bat)
{
	char *path;
	int err, fd;
	size_t size;
	uint32_t *table;
	vhdi_bat_header_t header;

	table = NULL;

	fd = open(name, O_RDONLY | O_LARGEFILE);
	if (fd == -1)
		return -errno;

	err = vhdi_bat_load_header(fd, &header);
	if (err)
		goto out;

	size = header.vhd_blocks * sizeof(uint32_t);
	table = malloc(size);
	if (!table) {
		err = -ENOMEM;
		goto out;
	}

	err = lseek64(fd, header.table_offset, SEEK_SET);
	if (err == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	err = read(fd, table, size);
	if (err != size) {
		err = (errno ? -errno : -EIO);
		goto out;
	}

	path = vhdi_path_expand(name, &header.vhd_path, &err);
	if (err)
		goto out;
	strcpy(bat->vhd_path, path);
	free(path);

	err = access(bat->vhd_path, F_OK);
	if (err == -1) {
		err = -errno;
		goto out;
	}

	path = vhdi_path_expand(name, &header.index_path, &err);
	if (err)
		goto out;
	strcpy(bat->index_path, path);
	free(path);

	err = access(bat->index_path, F_OK);
	if (err == -1) {
		err = -errno;
		goto out;
	}

	path = vhdi_path_expand(name, &header.file_table_path, &err);
	if (err)
		goto out;
	strcpy(bat->file_table_path, path);
	free(path);

	err = access(bat->file_table_path, F_OK);
	if (err == -1) {
		err = -errno;
		goto out;
	}

	bat->vhd_blocks     = header.vhd_blocks;
	bat->vhd_block_size = header.vhd_block_size;
	bat->table          = table;

	err = 0;

out:
	close(fd);
	if (err) {
		free(table);
		memset(bat, 0, sizeof(vhdi_bat_t));
	}

	return err;
}

int
vhdi_bat_write(const char *name, vhdi_bat_t *bat)
{
	int err, fd;
	size_t size;
	vhdi_bat_header_t header;

	fd = open(name, O_RDWR | O_LARGEFILE);
	if (fd == -1)
		return -errno;

	err = vhdi_bat_load_header(fd, &header);
	if (err)
		goto out;

	if (header.vhd_blocks != bat->vhd_blocks ||
	    header.vhd_block_size != bat->vhd_block_size) {
		err = -EINVAL;
		goto out;
	}

	err = lseek64(fd, header.table_offset, SEEK_SET);
	if (err == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	size = bat->vhd_blocks * sizeof(uint32_t);
	err = write(fd, bat->table, size);
	if (err != size) {
		err = (errno ? -errno : -EIO);
		goto out;
	}

	err = 0;

out:
	close(fd);
	return err;
}

int
vhdi_file_table_create(const char *file)
{
	int err, fd;
	off64_t off;
	vhdi_file_table_header_t header;

	memset(&header, 0, sizeof(vhdi_file_table_header_t));

	err = access(file, F_OK);
	if (!err || errno != ENOENT)
		return (err ? err : -EEXIST);

	off = vhd_bytes_padded(sizeof(vhdi_file_table_header_t));

	header.files        = 0;
	header.table_offset = off;
	memcpy(header.cookie,
	       VHDI_FILE_TABLE_HEADER_COOKIE, sizeof(header.cookie));

	vhdi_file_table_header_out(&header);

	fd = open(file, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd == -1)
		return -errno;

	err = write(fd, &header, sizeof(vhdi_file_table_header_t));
	if (err != sizeof(vhdi_file_table_header_t)) {
		err = (errno ? -errno : -EIO);
		goto out;
	}

	err = 0;

out:
	close(fd);
	return err;
}

int
vhdi_file_table_load(const char *name, vhdi_file_table_t *table)
{
	off64_t off;
	size_t size;
	int err, i, fd;
	vhdi_file_table_header_t header;
	vhdi_file_table_entry_t *entries;

	entries = NULL;

	fd = open(name, O_RDONLY | O_LARGEFILE);
	if (fd == -1)
		return -errno;

	err = vhdi_file_table_load_header(fd, &header);
	if (err)
		goto out;

	if (!header.files)
		goto out;

	table->table = calloc(header.files, sizeof(vhdi_file_ref_t));
	if (!table->table) {
		err = -ENOMEM;
		goto out;
	}

	off = header.table_offset;
	err = lseek64(fd, off, SEEK_SET);
	if (err == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	size = header.files * sizeof(vhdi_file_table_entry_t);
	entries = calloc(header.files, sizeof(vhdi_file_table_entry_t));
	if (!entries) {
		err = -ENOMEM;
		goto out;
	}

	err = read(fd, entries, size);
	if (err != size) {
		err = (errno ? -errno : -EIO);
		goto out;
	}

	for (i = 0; i < header.files; i++) {
		vhdi_file_table_entry_t *entry;

		entry = entries + i;
		vhdi_file_table_entry_in(entry);

		err = vhdi_file_table_entry_validate(entry);
		if (err)
			goto out;

		table->table[i].path = vhdi_path_expand(name,
							&entry->p, &err);
		if (err)
			goto out;

		err = vhdi_file_table_entry_validate_vhd(entry,
							 table->table[i].path);
		if (err)
			goto out;

		table->table[i].file_id       = entry->file_id;
		table->table[i].vhd_timestamp = entry->vhd_timestamp;
		uuid_copy(table->table[i].vhd_uuid, entry->vhd_uuid);
	}

	err = 0;
	table->entries = header.files;

out:
	close(fd);
	free(entries);
	if (err) {
		if (table->table) {
			for (i = 0; i < header.files; i++)
				free(table->table[i].path);
			free(table->table);
		}
		memset(table, 0, sizeof(vhdi_file_table_t));
	}

	return err;
}

static int
vhdi_file_table_next_fid(const char *name,
			 const char *file, vhdi_file_id_t *fid)
{
	int i, err;
	char *path;
	vhdi_file_id_t max;
	vhdi_file_table_t files;

	max  = 0;
	path = NULL;

	err = vhdi_file_table_load(name, &files);
	if (err)
		return err;

	path = realpath(file, NULL);
	if (!path) {
		err = -errno;
		goto out;
	}

	for (i = 0; i < files.entries; i++) {
		if (!strcmp(path, files.table[i].path)) {
			err = -EEXIST;
			goto out;
		}

		max = MAX(max, files.table[i].file_id);
	}

	*fid = max + 1;
	err  = 0;

out:
	vhdi_file_table_free(&files);
	free(path);

	return err;
}

static inline int
vhdi_file_table_entry_initialize(vhdi_file_table_entry_t *entry,
				 const char *file_table, const char *file,
				 vhdi_file_id_t fid)
{
	int err;
	struct stat stats;
	vhd_context_t vhd;

	memset(entry, 0, sizeof(vhdi_file_table_entry_t));

	err = stat(file, &stats);
	if (err == -1)
		return -errno;

	entry->file_id       = fid;
	entry->vhd_timestamp = vhd_time(stats.st_mtime);

	err = vhd_open(&vhd, file, VHD_OPEN_RDONLY);
	if (err)
		goto out;

	err = vhd_get_footer(&vhd);
	if (err) {
		vhd_close(&vhd);
		goto out;
	}

	uuid_copy(entry->vhd_uuid, vhd.footer.uuid);

	vhd_close(&vhd);

	err = vhdi_copy_path_to(&entry->p, file_table, file);
	if (err)
		goto out;

	err = 0;

out:
	if (err)
		memset(entry, 0, sizeof(vhdi_file_table_entry_t));

	return err;
}

int
vhdi_file_table_add(const char *name, const char *file, vhdi_file_id_t *_fid)
{
	off64_t off;
	size_t size;
	vhdi_file_id_t fid;
	int err, i, fd, len;
	vhdi_file_table_entry_t entry;
	vhdi_file_table_header_t header;

	off   = 0;
	fid   = 0;
	*_fid = 0;

        len = strnlen(file, VHD_MAX_NAME_LEN - 1);
	if (len == VHD_MAX_NAME_LEN - 1)
		return -ENAMETOOLONG;

	err = vhdi_file_table_next_fid(name, file, &fid);
	if (err)
		return err;

	fd = open(name, O_RDWR | O_LARGEFILE);
	if (fd == -1)
		return -errno;

	err = vhdi_file_table_load_header(fd, &header);
	if (err)
		goto out;

	size = sizeof(vhdi_file_table_entry_t);
	off  = header.table_offset + size * header.files;

	err = lseek64(fd, off, SEEK_SET);
	if (err == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	err = vhdi_file_table_entry_initialize(&entry, name, file, fid);
	if (err)
		goto out;

	vhdi_file_table_entry_out(&entry);

	err = write(fd, &entry, size);
	if (err != size) {
		err = (errno ? -errno : -EIO);
		goto out;
	}

	header.files++;
	err = vhdi_file_table_write_header(fd, &header);
	if (err)
		goto out;

	err   = 0;
	*_fid = fid;

out:
	if (err && off) {
		int gcc = ftruncate(fd, off);
	}
	close(fd);

	return err;
}

void
vhdi_file_table_free(vhdi_file_table_t *table)
{
	int i;

	if (table->table) {
		for (i = 0; i < table->entries; i++)
			free(table->table[i].path);
		free(table->table);
	}

	memset(table, 0, sizeof(vhdi_file_table_t));
}
