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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libvhd.h"
#include "libvhd-index.h"

static void
usage(void)
{
	printf("usage: vhd-index <command>\n"
	       "commands:\n"
	       "\t   index: <-i index name> <-v vhd file>\n"
	       "\t summary: <-s index name> [-v vhd file [-b block]]\n");
	exit(-EINVAL);
}

typedef struct vhdi_name              vhdi_name_t;

struct vhdi_name {
	char                         *vhd;
	char                         *bat;

	char                         *base;
	char                         *index;
	char                         *files;
};

static int
vhd_index_get_name(const char *index, const char *vhd, vhdi_name_t *name)
{
	int err, len;

	memset(name, 0, sizeof(vhdi_name_t));

	len = strnlen(index, VHD_MAX_NAME_LEN);
	if (len + 5 >= VHD_MAX_NAME_LEN - 1)
		return -ENAMETOOLONG;

	if (vhd) {
		len = strnlen(vhd, VHD_MAX_NAME_LEN);
		if (len >= VHD_MAX_NAME_LEN - 1)
			return -ENAMETOOLONG;

		err = asprintf(&name->vhd, "%s", vhd);
		if (err == -1) {
			name->vhd = NULL;
			goto fail;
		}

		err = asprintf(&name->bat, "%s.bat", vhd);
		if (err == -1) {
			name->bat = NULL;
			goto fail;
		}
	}

	err = asprintf(&name->base, "%s", index);
	if (err == -1) {
		name->base = NULL;
		goto fail;
	}

	err = asprintf(&name->index, "%s.index", index);
	if (err == -1) {
		name->index = NULL;
		goto fail;
	}

	err = asprintf(&name->files, "%s.files", index);
	if (err == -1) {
		name->files = NULL;
		goto fail;
	}

	return 0;

fail:
	free(name->vhd);
	free(name->bat);
	free(name->base);
	free(name->index);
	free(name->files);

	return -ENOMEM;
}

static inline void
vhd_index_free_name(vhdi_name_t *name)
{
	free(name->vhd);
	free(name->bat);
	free(name->base);
	free(name->index);
	free(name->files);
}

static inline int
vhd_index_add_file_table_entry(vhdi_name_t *name, const char *file,
			       vhdi_file_table_t *files, vhdi_file_id_t *fid)
{
	int err;

	vhdi_file_table_free(files);

	err = vhdi_file_table_add(name->files, file, fid);
	if (err)
		return err;

	return vhdi_file_table_load(name->files, files);
}

static inline int
vhd_index_get_file_id(vhdi_name_t *name, const char *file,
		      vhdi_file_table_t *files, vhdi_file_id_t *fid)
{
	int i, err;
	char *path;

	path = realpath(file, NULL);
	if (!path)
		return -errno;

	for (i = 0; i < files->entries; i++)
		if (!strcmp(files->table[i].path, path)) {
			*fid = files->table[i].file_id;
			free(path);
			return 0;
		}

	free(path);
	return vhd_index_add_file_table_entry(name, file, files, fid);
}

static inline int
vhd_index_get_block(vhdi_context_t *vhdi, vhd_context_t *vhd,
		    uint32_t block, vhdi_block_t *vhdi_block)
{
	int i;

	if (block)
		return vhdi_read_block(vhdi, vhdi_block, block);

	vhdi_block->entries = vhd->spb;
	vhdi_block->table   = calloc(vhd->spb, sizeof(vhdi_entry_t));
	if (!vhdi_block->table)
		return -ENOMEM;

	for (i = 0; i < vhdi_block->entries; i++)
		vhdi_block->table[i].offset = DD_BLK_UNUSED;

	return 0;
}

static int
vhd_index_add_bat_entry(vhdi_name_t *name, vhdi_context_t *vhdi,
			vhdi_bat_t *bat, vhdi_file_table_t *files,
			vhd_context_t *vhd, uint32_t block, char *finished)
{
	char *map;
	vhdi_file_id_t fid;
	uint32_t i, count, off;
	vhdi_block_t vhdi_block;
	int err, update, append;

	fid    = 0;
	count  = 0;
	update = 0;
	append = (bat->table[block] == 0);

	if (vhd->bat.bat[block] == DD_BLK_UNUSED)
		return 0;

	err = vhd_index_get_block(vhdi, vhd, bat->table[block], &vhdi_block);
	if (err)
		return err;

	err = vhd_read_bitmap(vhd, block, &map);
	if (err)
		goto out;

	err = vhd_index_get_file_id(name, vhd->file, files, &fid);
	if (err)
		goto out;

	for (i = 0; i < vhd->spb; i++) {
		if (vhdi_block.table[i].file_id) {
			count++;
			continue;
		}

		if (!vhd_bitmap_test(vhd, map, i))
			continue;

		err = vhd_offset(vhd, (uint64_t)block * vhd->spb + i, &off);
		if (err)
			goto out;

		vhdi_block.table[i].file_id = fid;
		vhdi_block.table[i].offset  = off;
		count++;
		update++;
	}

	if (update) {
		if (append) {
			uint32_t location;

			err = vhdi_append_block(vhdi, &vhdi_block, &location);
			if (err)
				goto out;

			bat->table[block] = location;
		} else {
			err = vhdi_write_block(vhdi, &vhdi_block,
					       bat->table[block]);
			if (err)
				goto out;
		}
	}

	if (count == vhd->spb)
		*finished = 1;

	err = 0;

out:
	free(vhdi_block.table);
	free(map);

	return err;
}

static int
vhd_index_clone_bat_entry(vhdi_name_t *name, vhdi_context_t *vhdi,
			  vhdi_bat_t *bat, vhdi_file_table_t *files,
			  vhd_context_t *vhd, uint32_t block)
{
	char *map;
	int err, update;
	uint32_t i, off;
	vhdi_file_id_t fid;
	vhdi_block_t vhdi_block;

	fid    = 0;
	update = 0;

	if (vhd->bat.bat[block] == DD_BLK_UNUSED)
		return 0;

	err = vhd_index_get_block(vhdi, vhd, bat->table[block], &vhdi_block);
	if (err)
		return err;

	err = vhd_read_bitmap(vhd, block, &map);
	if (err)
		goto out;

	err = vhd_index_get_file_id(name, vhd->file, files, &fid);
	if (err)
		goto out;

	for (i = 0; i < vhd->spb; i++) {
		if (!vhd_bitmap_test(vhd, map, i))
			continue;

		err = vhd_offset(vhd, (uint64_t)block * vhd->spb + i, &off);
		if (err)
			goto out;

		vhdi_block.table[i].file_id = fid;
		vhdi_block.table[i].offset  = off;
		update++;
	}

	if (update) {
		uint32_t location;

		err = vhdi_append_block(vhdi, &vhdi_block, &location);
		if (err)
			goto out;

		bat->table[block] = location;
	}

	err = 0;

out:
	free(vhdi_block.table);
	free(map);

	return err;
}

static int
vhd_index_update_bat_entry(vhdi_name_t *name, vhdi_context_t *vhdi,
			   vhdi_bat_t *bat, vhdi_file_table_t *files,
			   vhd_context_t *vhd, uint32_t block)
{
	char *map;
	int err, update;
	uint32_t i, off;
	vhdi_file_id_t fid;
	vhdi_block_t vhdi_block;

	fid    = 0;
	update = 0;

	if (vhd->bat.bat[block] == DD_BLK_UNUSED)
		return 0;

	err = vhd_index_get_block(vhdi, vhd, bat->table[block], &vhdi_block);
	if (err)
		return err;

	err = vhd_read_bitmap(vhd, block, &map);
	if (err)
		goto out;

	err = vhd_index_get_file_id(name, vhd->file, files, &fid);
	if (err)
		goto out;

	for (i = 0; i < vhd->spb; i++) {
		if (!vhd_bitmap_test(vhd, map, i))
			continue;

		err = vhd_offset(vhd, (uint64_t)block * vhd->spb + i, &off);
		if (err)
			goto out;

		if (vhdi_block.table[i].file_id == fid &&
		    vhdi_block.table[i].offset  == off)
			continue;

		vhdi_block.table[i].file_id = fid;
		vhdi_block.table[i].offset  = off;
		update++;
	}

	if (update) {
		uint32_t location;

		err = vhdi_append_block(vhdi, &vhdi_block, &location);
		if (err)
			goto out;

		bat->table[block] = location;
	}

	err = 0;

out:
	free(vhdi_block.table);
	free(map);

	return err;
}

static int
vhd_index_add_bat(vhdi_name_t *name,
		  uint64_t vhd_blocks, uint32_t vhd_block_size)
{
	int err;
	vhdi_bat_t bat;
	vhd_context_t vhd;
	vhdi_context_t vhdi;
	vhdi_file_table_t files;
	char *vhd_file, *finished;
	uint32_t block, remaining;

	memset(&bat, 0, sizeof(vhdi_bat_t));
	memset(&files, 0, sizeof(vhdi_file_table_t));

	vhd_file           = NULL;
	finished           = NULL;
	bat.vhd_blocks     = vhd_blocks;
	bat.vhd_block_size = vhd_block_size;

	strcpy(bat.vhd_path, name->vhd);
	strcpy(bat.index_path, name->index);
	strcpy(bat.file_table_path, name->files);

	err = vhdi_open(&vhdi, name->index, O_RDWR);
	if (err)
		return err;

	err = vhdi_file_table_load(name->files, &files);
	if (err) {
		vhdi_close(&vhdi);
		return err;
	}

	err = vhdi_bat_create(name->bat, name->vhd, name->index, name->files);
	if (err)
		goto out;

	bat.table = calloc(vhd_blocks, sizeof(uint32_t));
	if (!bat.table) {
		err = -ENOMEM;
		goto out;
	}

	vhd_file = strdup(name->vhd);
	if (!vhd_file)
		goto out;

	remaining = vhd_blocks;
	finished  = calloc(remaining, sizeof(char));
	if (!finished) {
		err = -ENOMEM;
		goto out;
	}

	for (;;) {
		err = vhd_open(&vhd, vhd_file, VHD_OPEN_RDONLY);
		if (err)
			goto out;

		err = vhd_get_bat(&vhd);
		if (err)
			goto out_vhd;

		for (block = 0; block < vhd.bat.entries; block++) {
			if (finished[block])
				continue;

			err = vhd_index_add_bat_entry(name, &vhdi, &bat,
						      &files, &vhd, block,
						      &finished[block]);
			if (err)
				goto out_bat;

			if (finished[block])
				remaining--;
		}

		free(vhd_file);
		vhd_file = NULL;

		if (!remaining || vhd.footer.type != HD_TYPE_DIFF) {
			vhd_put_bat(&vhd);
			vhd_close(&vhd);
			break;
		}

		err = vhd_parent_locator_get(&vhd, &vhd_file);
		if (err)
			goto out_bat;

	out_bat:
		vhd_put_bat(&vhd);
	out_vhd:
		vhd_close(&vhd);
		if (err)
			goto out;
	} 

	err = vhdi_bat_write(name->bat, &bat);
	if (err)
		goto out;

	err = 0;

out:
	if (err)
		unlink(name->bat);

	vhdi_file_table_free(&files);
	vhdi_close(&vhdi);
	free(bat.table);
	free(finished);
	free(vhd_file);

	return err;
}

static int
vhd_index_clone_bat(vhdi_name_t *name, const char *parent)
{
	int err;
	char *pbat;
	uint32_t block;
	vhdi_bat_t bat;
	vhd_context_t vhd;
	vhdi_context_t vhdi;
	vhdi_file_table_t files;

	memset(&bat, 0, sizeof(vhdi_bat_t));
	memset(&files, 0, sizeof(vhdi_file_table_t));

	err = asprintf(&pbat, "%s.bat", parent);
	if (err == -1)
		return -ENOMEM;

	err = access(pbat, R_OK);
	if (err == -1) {
		free(pbat);
		return -errno;
	}

	err = vhdi_open(&vhdi, name->index, O_RDWR);
	if (err)
		goto out;

	err = vhdi_bat_load(pbat, &bat);
	if (err)
		goto out_vhdi;

	err = vhdi_file_table_load(name->files, &files);
	if (err)
		goto out_vhdi;

	err = vhdi_bat_create(name->bat, name->vhd, name->index, name->files);
	if (err)
		goto out_ft;

	err = vhdi_bat_write(name->bat, &bat);
	if (err)
		goto out_ft;

	err = vhd_open(&vhd, name->vhd, VHD_OPEN_RDONLY);
	if (err)
		goto out_ft;

	err = vhd_get_bat(&vhd);
	if (err)
		goto out_vhd;

	for (block = 0; block < vhd.bat.entries; block++) {
		err = vhd_index_clone_bat_entry(name, &vhdi, &bat,
						&files, &vhd, block);
		if (err)
			goto out_bat;
	}

	err = vhdi_bat_write(name->bat, &bat);
	if (err)
		goto out_bat;

	err = 0;

out_bat:
	vhd_put_bat(&vhd);
out_vhd:
	vhd_close(&vhd);
out_ft:
	vhdi_file_table_free(&files);
out_vhdi:
	vhdi_close(&vhdi);
out:
	if (err)
		unlink(name->bat);
	free(bat.table);
	free(pbat);
	return err;
}

static int
vhd_index_update_bat(vhdi_name_t *name)
{
	int err;
	uint32_t block;
	vhdi_bat_t bat;
	vhd_context_t vhd;
	vhdi_context_t vhdi;
	vhdi_file_table_t files;

	memset(&bat, 0, sizeof(vhdi_bat_t));
	memset(&files, 0, sizeof(vhdi_file_table_t));

	err = access(name->bat, R_OK);
	if (err == -1)
		return -errno;

	err = vhdi_open(&vhdi, name->index, O_RDWR);
	if (err)
		goto out;

	err = vhdi_bat_load(name->bat, &bat);
	if (err)
		goto out_vhdi;

	err = vhdi_file_table_load(name->files, &files);
	if (err)
		goto out_vhdi;

	err = vhd_open(&vhd, name->vhd, VHD_OPEN_RDONLY);
	if (err)
		goto out_ft;

	err = vhd_get_bat(&vhd);
	if (err)
		goto out_bat;

	for (block = 0; block < vhd.bat.entries; block++) {
		err = vhd_index_update_bat_entry(name, &vhdi, &bat,
						 &files, &vhd, block);
		if (err)
			goto out_bat;
	}

	err = vhdi_bat_write(name->bat, &bat);
	if (err)
		goto out_bat;

	err = 0;

out_bat:
	vhd_put_bat(&vhd);
out_vhd:
	vhd_close(&vhd);
out_ft:
	vhdi_file_table_free(&files);
out_vhdi:
	vhdi_close(&vhdi);
out:
	free(bat.table);
	return err;
}

static int
vhd_index_create(vhdi_name_t *name)
{
	int err, len;
	vhd_context_t ctx;
	uint32_t block_size;

	if (!access(name->index, F_OK) || !access(name->files, F_OK))
		return -EEXIST;

	err = vhd_open(&ctx, name->vhd, VHD_OPEN_RDONLY);
	if (err)
		return err;

	err = vhd_get_header(&ctx);
	if (err) {
		vhd_close(&ctx);
		return err;
	}

	block_size = ctx.header.block_size;
	vhd_close(&ctx);

	err = vhdi_create(name->index, block_size);
	if (err)
		goto out;

	err = vhdi_file_table_create(name->files);
	if (err)
		goto out;

	err = 0;

out:
	if (err) {
		unlink(name->index);
		unlink(name->files);
	}

	return err;
}

static int
vhd_index(vhdi_name_t *name)
{
	char *parent;
	vhd_context_t ctx;
	uint64_t vhd_blocks;
	uint32_t vhd_block_size;
	int err, len, new_index, new_bat;

	parent    = NULL;
	new_bat   = 0;
	new_index = 0;

	/* find vhd's parent -- we only index read-only vhds */
	err = vhd_open(&ctx, name->vhd, VHD_OPEN_RDONLY);
	if (err)
		return err;

	err = vhd_parent_locator_get(&ctx, &parent);
	vhd_close(&ctx);

	if (err)
		return err;

	/* update name to point to parent */
	free(name->vhd);
	name->vhd = parent;
	parent = NULL;

	free(name->bat);
	err = asprintf(&name->bat, "%s.bat", name->vhd);
	if (err == -1) {
		name->bat = NULL;
		return -ENOMEM;
	}

	/* create index if it doesn't already exist */
	err = access(name->index, R_OK | W_OK);
	if (err == -1 && errno == ENOENT) {
		new_index = 1;
		err = vhd_index_create(name);
	}

	if (err)
		return err;

	/* get basic vhd info */
	err = vhd_open(&ctx, name->vhd, VHD_OPEN_RDONLY);
	if (err)
		goto out;

	err = vhd_get_header(&ctx);
	if (err) {
		vhd_close(&ctx);
		goto out;
	}

	vhd_blocks     = ctx.header.max_bat_size;
	vhd_block_size = ctx.header.block_size;

	if (vhd_parent_locator_get(&ctx, &parent))
		parent = NULL;

	vhd_close(&ctx);

	/* update existing bat if it exists */
	err = vhd_index_update_bat(name);
	if (err != -ENOENT)
		goto out;

	new_bat = 1;

	if (parent) {
		/* clone parent bat if it exists */
		err = vhd_index_clone_bat(name, parent);
		if (err != -ENOENT)
			goto out;
	}

	/* create new bat from scratch */
	err = vhd_index_add_bat(name, vhd_blocks, vhd_block_size);
	if (err)
		goto out;

	err = 0;

out:
	if (err) {
		if (new_bat)
			unlink(name->bat);
		if (new_index) {
			unlink(name->index);
			unlink(name->files);
		}
	}
	free(parent);
	return err;
}

static void
vhd_index_print_summary(vhdi_name_t *name,
			uint32_t block_size, vhdi_file_table_t *files)
{
	int i;
	char time[26], uuid[37];

	printf("VHD INDEX          : %s\n", name->index);
	printf("--------------------\n");
	printf("block size         : %u\n", block_size);
	printf("files              : %d\n", files->entries);

	printf("\n");
	for (i = 0; i < files->entries; i++) {
		uuid_unparse(files->table[i].vhd_uuid, uuid);
		vhd_time_to_string(files->table[i].vhd_timestamp, time);

		printf("        fid 0x%04x : %s, %s, %s\n",
		       files->table[i].file_id, files->table[i].path, uuid, time);
	}

	printf("\n");
}

static inline void
vhd_index_print_bat_header(const char *name, vhdi_bat_t *bat)
{
	printf("VHD INDEX BAT      : %s\n", name);
	printf("--------------------\n");
	printf("blocks             : %"PRIu64"\n", bat->vhd_blocks);
	printf("block size         : %u\n", bat->vhd_block_size);
	printf("vhd path           : %s\n", bat->vhd_path);
	printf("index path         : %s\n", bat->index_path);
	printf("file table path    : %s\n", bat->file_table_path);
}

static int
vhd_index_print_vhd_summary(vhdi_name_t *name)
{
	int err;
	uint32_t i;
	vhdi_bat_t bat;

	err = vhdi_bat_load(name->bat, &bat);
	if (err)
		return err;

	vhd_index_print_bat_header(name->bat, &bat);

	printf("\n");
	for (i = 0; i < bat.vhd_blocks; i++)
		printf("      block 0x%04x : offset 0x%08x\n", i, bat.table[i]);

	free(bat.table);
	return 0;
}

static int
vhd_index_print_vhd_block_summary(vhdi_name_t *name, uint32_t block)
{
	int err;
	uint32_t i;
	uint32_t off;
	vhdi_bat_t bat;
	vhdi_context_t vhdi;
	vhdi_block_t vhdi_block;

	err = vhdi_bat_load(name->bat, &bat);
	if (err)
		return err;

	vhd_index_print_bat_header(name->bat, &bat);

	if (block > bat.vhd_blocks) {
		printf("block %u past end of bat (%"PRIu64")\n",
		       block, bat.vhd_blocks);
		err = -EINVAL;
		goto out;
	}

	off = bat.table[block];
	if (off == DD_BLK_UNUSED) {
		printf("block %u is unallocated\n", block);
		err = 0;
		goto out;
	}

	err = vhdi_open(&vhdi, name->index, O_RDWR);
	if (err)
		goto out;

	err = vhdi_read_block(&vhdi, &vhdi_block, off);
	vhdi_close(&vhdi);
	if (err)
		goto out;

	printf("\nBLOCK 0x%08x\n", block);
	for (i = 0; i < vhdi_block.entries; i++)
		printf("        sec 0x%04x : fid 0x%04x, offset 0x%08x\n", i,
		       vhdi_block.table[i].file_id,
		       vhdi_block.table[i].offset);

	free(vhdi_block.table);
	err = 0;

out:
	free(bat.table);
	return err;
}

static int
vhd_index_summary(vhdi_name_t *name, uint32_t block)
{
	int err;
	uint32_t block_size;
	vhdi_context_t vhdi;
	vhdi_file_table_t files;

	err = vhdi_open(&vhdi, name->index, O_RDWR);
	if (err)
		return err;

	block_size = vhdi.vhd_block_size;
	vhdi_close(&vhdi);

	err = vhdi_file_table_load(name->files, &files);
	if (err)
		return err;

	vhd_index_print_summary(name, block_size, &files);

	if (name->vhd) {
		if (block == (uint32_t)-1)
			err = vhd_index_print_vhd_summary(name);
		else
			err = vhd_index_print_vhd_block_summary(name, block);

		if (err)
			goto out;
	}

	err = 0;

out:
	vhdi_file_table_free(&files);
	return err;
}

int
main(int argc, char *argv[])
{
	int err;
	uint32_t block;
	vhdi_name_t name;
	char *vhd, *index;
	int c, update, summary;

	vhd     = NULL;
	index   = NULL;
	block   = (uint32_t)-1;
	update  = 0;
	summary = 0;

	while ((c = getopt(argc, argv, "i:v:s:b:h")) != -1) {
		switch (c) {
		case 'i':
			index   = optarg;
			update  = 1;
			break;

		case 'v':
			vhd     = optarg;
			break;

		case 's':
			index   = optarg;
			summary = 1;
			break;

		case 'b':
			block   = strtoul(optarg, NULL, 10);
			break;

		default:
			usage();
		}
	}

	if (optind != argc)
		usage();

	if (!(update ^ summary))
		usage();

	if (block != (uint32_t)-1 && (!summary || !vhd))
		usage();

	err = vhd_index_get_name(index, vhd, &name);
	if (err)
		goto out;

	if (summary)
		err = vhd_index_summary(&name, block);
	else if (update) {
		if (!vhd)
			usage();

		err = vhd_index(&name);
	}

out:
	vhd_index_free_name(&name);
	return -err;
}
