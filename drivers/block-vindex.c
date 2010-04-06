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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include "tapdisk.h"
#include "tapdisk-utils.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-interface.h"

#include "libvhd.h"
#include "libvhd-index.h"

#define DBG(_level, _f, _a...)       tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...)         tlog_error(_err, _f, ##_a)
#define WARN(_f, _a...)              tlog_write(TLOG_WARN, _f, ##_a)

#define ASSERT(condition)					\
	if (!(condition)) {					\
		WARN("FAILED ASSERTION: '%s'\n", #condition);	\
		td_panic();					\
	}

#define VHD_INDEX_FILE_POOL_SIZE     12
#define VHD_INDEX_CACHE_SIZE         4
#define VHD_INDEX_REQUESTS           (TAPDISK_DATA_REQUESTS + VHD_INDEX_CACHE_SIZE)

#define VHD_INDEX_BLOCK_READ_PENDING 0x0001
#define VHD_INDEX_BLOCK_VALID        0x0002

#define VHD_INDEX_BAT_CLEAR          0
#define VHD_INDEX_BIT_CLEAR          1
#define VHD_INDEX_BIT_SET            2
#define VHD_INDEX_CACHE_MISS         3
#define VHD_INDEX_META_READ_PENDING  4

typedef struct vhd_index             vhd_index_t;
typedef struct vhd_index_block       vhd_index_block_t;
typedef struct vhd_index_request     vhd_index_request_t;
typedef struct vhd_index_file_ref    vhd_index_file_ref_t;

struct vhd_index_request {
	off64_t                      off;
	td_request_t                 treq;
	vhd_index_t                 *index;
	struct tiocb                 tiocb;
	struct list_head             next;
	vhd_index_file_ref_t        *file;
};

struct vhd_index_block {
	uint64_t                     blk;
	uint32_t                     seqno;
	td_flag_t                    state;
	vhdi_block_t                 vhdi_block;
	int                          table_size;
	struct list_head             queue;
	vhd_index_request_t          req;
};

struct vhd_index_file_ref {
	int                          fd;
	vhdi_file_id_t               fid;
	uint32_t                     seqno;
	uint32_t                     refcnt;
};

struct vhd_index {
	char                        *name;

	vhdi_bat_t                   bat;
	vhdi_context_t               vhdi;
	vhdi_file_table_t            files;

	vhd_index_file_ref_t         fds[VHD_INDEX_FILE_POOL_SIZE];

	vhd_index_block_t           *cache[VHD_INDEX_CACHE_SIZE];

	int                          cache_free_cnt;
	vhd_index_block_t           *cache_free_list[VHD_INDEX_CACHE_SIZE];
	vhd_index_block_t            cache_list[VHD_INDEX_CACHE_SIZE];

	int                          requests_free_cnt;
	vhd_index_request_t         *requests_free_list[VHD_INDEX_REQUESTS];
	vhd_index_request_t          requests_list[VHD_INDEX_REQUESTS];

	td_driver_t                 *driver;
};

static void vhd_index_complete_meta_read(void *, struct tiocb *, int);
static void vhd_index_complete_data_read(void *, struct tiocb *, int);

#define vhd_index_block_for_each_request(_block, _req, _tmp)		\
	list_for_each_entry_safe((_req), (_tmp), &(_block)->queue, next)

static inline void
vhd_index_initialize_request(vhd_index_request_t *req)
{
	memset(req, 0, sizeof(vhd_index_request_t));
	INIT_LIST_HEAD(&req->next);
}

static inline void
vhd_index_initialize_block(vhd_index_block_t *block)
{
	char *buf;

	block->blk   = 0;
	block->state = 0;
	INIT_LIST_HEAD(&block->queue);
	vhd_index_initialize_request(&block->req);
	memset(block->vhdi_block.table, 0, block->table_size);
}

static void
vhd_index_init(vhd_index_t *index)
{
	int i;

	memset(index, 0, sizeof(vhd_index_t));

	index->cache_free_cnt = VHD_INDEX_CACHE_SIZE;
	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++) {
		index->cache_free_list[i] = index->cache_list + i;
		vhd_index_initialize_block(index->cache_free_list[i]);
	}

	index->requests_free_cnt = VHD_INDEX_REQUESTS;
	for (i = 0; i < VHD_INDEX_REQUESTS; i++) {
		index->requests_free_list[i] = index->requests_list + i;
		vhd_index_initialize_request(index->requests_free_list[i]);
	}

	for (i = 0; i < VHD_INDEX_FILE_POOL_SIZE; i++)
		index->fds[i].fd = -1;
}

static int
vhd_index_allocate_cache(vhd_index_t *index)
{
	char *buf;
	int i, err;
	size_t size;

	size = vhd_bytes_padded(index->vhdi.spb * sizeof(vhdi_entry_t));

	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++) {
		err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
		if (err)
			goto fail;

		memset(buf, 0, size);
		index->cache_list[i].vhdi_block.table   = (vhdi_entry_t *)buf;
		index->cache_list[i].vhdi_block.entries = index->vhdi.spb;
		index->cache_list[i].table_size         = size;
	}

	return 0;

fail:
	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++) {
		free(index->cache_list[i].vhdi_block.table);
		index->cache_list[i].vhdi_block.table = NULL;
	}

	return -ENOMEM;
}

static void
vhd_index_free(vhd_index_t *index)
{
	int i;

	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++)
		free(index->cache_list[i].vhdi_block.table);

	for (i = 0; i < VHD_INDEX_FILE_POOL_SIZE; i++)
		if (index->fds[i].fd != -1)
			close(index->fds[i].fd);

	vhdi_file_table_free(&index->files);
	free(index->bat.table);
	free(index->name);
}

static int
vhd_index_load(vhd_index_t *index)
{
	int err;

	err = vhdi_bat_load(index->name, &index->bat);
	if (err)
		return err;

	err = vhdi_open(&index->vhdi,
			index->bat.index_path,
			O_RDONLY | O_DIRECT | O_LARGEFILE);
	if (err)
		goto fail;

	err = vhdi_file_table_load(index->bat.file_table_path, &index->files);
	if (err) {
		vhdi_close(&index->vhdi);
		goto fail;
	}

	return 0;

fail:
	free(index->bat.table);
	memset(&index->bat, 0, sizeof(vhdi_bat_t));
	memset(&index->vhdi, 0, sizeof(vhdi_context_t));
	memset(&index->files, 0, sizeof(vhdi_file_table_t));
	return err;
}

static int
vhd_index_open(td_driver_t *driver, const char *name, td_flag_t flags)
{
	int err;
	vhd_index_t *index;

	index = (vhd_index_t *)driver->data;

	vhd_index_init(index);

	index->name = strdup(name);
	if (!index->name)
		return -ENOMEM;

	err = vhd_index_load(index);
	if (err) {
		free(index->name);
		return err;
	}

	err = vhd_index_allocate_cache(index);
	if (err) {
		vhd_index_free(index);
		return err;
	}

	driver->info.size = index->bat.vhd_blocks * index->bat.vhd_block_size;
	driver->info.sector_size = VHD_SECTOR_SIZE;
	driver->info.info = 0;

	index->driver = driver;

	DPRINTF("opened vhd index %s\n", name);

	return 0;
}

static int
vhd_index_close(td_driver_t *driver)
{
	vhd_index_t *index;

	index = (vhd_index_t *)driver->data;
	vhdi_close(&index->vhdi);

	DPRINTF("closed vhd index %s\n", index->name);

	vhd_index_free(index);

	return 0;
}

static inline void
vhd_index_touch_file_ref(vhd_index_t *index, vhd_index_file_ref_t *ref)
{
	int i;

	if (++ref->seqno == 0xFFFFFFFF)
		for (i = 0; i < VHD_INDEX_FILE_POOL_SIZE; i++)
			index->fds[i].seqno >>= 1;
}

static inline void
vhd_index_get_file_ref(vhd_index_file_ref_t *ref)
{
	++ref->refcnt;
}

static inline void
vhd_index_put_file_ref(vhd_index_file_ref_t *ref)
{
	--ref->refcnt;
}

static inline vhd_index_file_ref_t *
vhd_index_find_lru_file_ref(vhd_index_t *index)
{
	int i;
	uint32_t min;
	vhd_index_file_ref_t *lru;

	lru = NULL;
	min = (uint32_t)-1;

	for (i = 1; i < VHD_INDEX_FILE_POOL_SIZE; i++) {
		if (index->fds[i].refcnt)
			continue;

		if (!lru || index->fds[i].seqno < min) {
			min = index->fds[i].seqno;
			lru = index->fds + i;
		}
	}

	return lru;
}

static inline int
vhd_index_open_file(vhd_index_t *index,
		    vhdi_file_id_t id, vhd_index_file_ref_t *ref)
{
	int i;
	char *path;

	path = NULL;

	for (i = 0; i < index->files.entries; i++)
		if (index->files.table[i].file_id == id) {
			path = index->files.table[i].path;
			break;
		}

	if (!path)
		return -ENOENT;

	ref->fd = open(path, O_RDONLY | O_DIRECT | O_LARGEFILE);
	if (ref->fd == -1)
		return -errno;

	ref->fid    = id;
	ref->refcnt = 0;

	return 0;
}

static int
vhd_index_get_file(vhd_index_t *index,
		   vhdi_file_id_t id, vhd_index_file_ref_t **ref)
{
	int i, err;
	vhd_index_file_ref_t *lru;

	*ref = NULL;

	for (i = 0; i < VHD_INDEX_FILE_POOL_SIZE; i++)
		if (id == index->fds[i].fid) {
			*ref = index->fds + i;
			vhd_index_touch_file_ref(index, *ref);
			vhd_index_get_file_ref(*ref);
			return 0;
		}

	lru = vhd_index_find_lru_file_ref(index);
	if (!lru)
		return -EBUSY;

	if (lru->fd != -1)
		close(lru->fd);

	err = vhd_index_open_file(index, id, lru);
	if (err)
		goto fail;

	vhd_index_touch_file_ref(index, lru);
	vhd_index_get_file_ref(lru);
	*ref = lru;
	return 0;

fail:
	lru->fd     = -1;
	lru->fid    = 0;
	lru->refcnt = 0;
	return err;
}

static inline vhd_index_request_t *
vhd_index_allocate_request(vhd_index_t *index)
{
	vhd_index_request_t *req;

	if (index->requests_free_cnt <= 0)
		return NULL;

	req = index->requests_free_list[--index->requests_free_cnt];
	ASSERT(!req->index);

	return req;
}

static inline void
vhd_index_free_request(vhd_index_t *index, vhd_index_request_t *req)
{
	list_del(&req->next);
	vhd_index_initialize_request(req);
	index->requests_free_list[index->requests_free_cnt++] = req;
}

static inline int
vhd_index_block_valid(vhd_index_block_t *block)
{
	return (!td_flag_test(block->state, VHD_INDEX_BLOCK_READ_PENDING) &&
		td_flag_test(block->state, VHD_INDEX_BLOCK_VALID));
}

static inline void
vhd_index_touch_block(vhd_index_t *index, vhd_index_block_t *block)
{
	int i;

	if (++block->seqno == 0xFFFFFFFF)
		for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++)
			index->cache_list[i].seqno >>= 1;
}

static inline vhd_index_block_t *
vhd_index_get_lru_block(vhd_index_t *index)
{
	int i, idx;
	uint32_t min;
	vhd_index_block_t *block, *lru;

	lru = NULL;
	min = (uint32_t)-1;
	idx = 0;

	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++) {
		block = index->cache[i];

		if (!block)
			continue;

		if (td_flag_test(block->state, VHD_INDEX_BLOCK_READ_PENDING))
			continue;

		if (!lru || block->seqno < min) {
			lru = block;
			min = block->seqno;
			idx = i;
		}
	}

	if (lru)
		index->cache[idx] = NULL;

	return lru;
}

static inline int
vhd_index_allocate_block(vhd_index_t *index, vhd_index_block_t **block)
{
	vhd_index_block_t *b;

	*block = NULL;

	if (index->cache_free_cnt > 0)
		b = index->cache_free_list[--index->cache_free_cnt];
	else {
		b = vhd_index_get_lru_block(index);
		if (!b)
			return -EBUSY;
	}

	vhd_index_initialize_block(b);
	vhd_index_touch_block(index, b);
	*block = b;

	return 0;
}

static int
vhd_index_install_block(vhd_index_t *index,
			vhd_index_block_t **block, uint32_t blk)
{
	int i, err;
	vhd_index_block_t *b;

	*block = NULL;

	err = vhd_index_allocate_block(index, &b);
	if (err)
		return err;

	b->blk = blk;

	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++)
		if (!index->cache[i]) {
			index->cache[i] = b;
			break;
		}

	ASSERT(i < VHD_INDEX_CACHE_SIZE);
	*block = b;

	return 0;
}

static inline vhd_index_block_t *
vhd_index_get_block(vhd_index_t *index, uint32_t blk)
{
	int i;
	vhd_index_block_t *block;

	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++) {
		block = index->cache[i];
		if (!block)
			continue;

		if (block->blk == blk)
			return block;
	}

	return NULL;
}

static int
vhd_index_read_cache(vhd_index_t *index, uint64_t sector)
{
	uint32_t blk, sec;
	vhd_index_block_t *block;

	blk = sector / index->vhdi.spb;

	if (blk >= index->bat.vhd_blocks)
		return -EINVAL;

	if (index->bat.table[blk] == DD_BLK_UNUSED)
		return VHD_INDEX_BAT_CLEAR;

	block = vhd_index_get_block(index, blk);
	if (!block)
		return VHD_INDEX_CACHE_MISS;

	vhd_index_touch_block(index, block);

	if (td_flag_test(block->state, VHD_INDEX_BLOCK_READ_PENDING))
		return VHD_INDEX_META_READ_PENDING;

	sec = sector % index->vhdi.spb;
	if (block->vhdi_block.table[sec].offset == DD_BLK_UNUSED)
		return VHD_INDEX_BIT_CLEAR;

	return VHD_INDEX_BIT_SET;
}

static int
vhd_index_read_cache_span(vhd_index_t *index,
			  uint64_t sector, int secs, int value)
{
	int i;
	uint32_t blk, sec;
	vhd_index_block_t *block;

	blk = sector / index->vhdi.spb;
	sec = sector % index->vhdi.spb;

	ASSERT(blk < index->bat.vhd_blocks);

	block = vhd_index_get_block(index, blk);
	ASSERT(block && vhd_index_block_valid(block));

	for (i = 0; i < secs && i + sec < index->vhdi.spb; i++)
		if (value ^
		    (block->vhdi_block.table[sec + i].offset != DD_BLK_UNUSED))
			break;

	return i;
}

static int
vhd_index_schedule_meta_read(vhd_index_t *index, uint32_t blk)
{
	int err;
	off64_t offset;
	vhd_index_block_t *block;
	vhd_index_request_t *req;

	ASSERT(index->bat.table[blk] != DD_BLK_UNUSED);

	block = vhd_index_get_block(index, blk);
	if (!block) {
		err = vhd_index_install_block(index, &block, blk);
		if (err)
			return err;
	}

	offset         = vhd_sectors_to_bytes(index->bat.table[blk]);

	req            = &block->req;
	req->index     = index;
	req->treq.sec  = blk * index->vhdi.spb;
	req->treq.secs = block->table_size >> VHD_SECTOR_SHIFT;

	td_prep_read(&req->tiocb, index->vhdi.fd,
		     (char *)block->vhdi_block.table, block->table_size,
		     offset, vhd_index_complete_meta_read, req);
	td_queue_tiocb(index->driver, &req->tiocb);

	td_flag_set(block->state, VHD_INDEX_BLOCK_READ_PENDING);

	return 0;
}

static int
vhd_index_schedule_data_read(vhd_index_t *index, td_request_t treq)
{
	int i, err;
	size_t size;
	off64_t offset;
	uint32_t blk, sec;
	vhd_index_block_t *block;
	vhd_index_request_t *req;
	vhd_index_file_ref_t *file;

	blk   = treq.sec / index->vhdi.spb;
	sec   = treq.sec % index->vhdi.spb;
	block = vhd_index_get_block(index, blk);

	ASSERT(block && vhd_index_block_valid(block));
	for (i = 0; i < treq.secs; i++) {
		ASSERT(block->vhdi_block.table[sec + i].file_id != 0);
		ASSERT(block->vhdi_block.table[sec + i].offset != DD_BLK_UNUSED);
	}

	req = vhd_index_allocate_request(index);
	if (!req)
		return -EBUSY;

	err = vhd_index_get_file(index,
				 block->vhdi_block.table[sec].file_id, &file);
	if (err) {
		vhd_index_free_request(index, req);
		return err;
	}

	size       = vhd_sectors_to_bytes(treq.secs);
	offset     = vhd_sectors_to_bytes(block->vhdi_block.table[sec].offset);

	req->file  = file;
	req->treq  = treq;
	req->index = index;
	req->off   = offset;

	td_prep_read(&req->tiocb, file->fd, treq.buf, size, offset,
		     vhd_index_complete_data_read, req);
	td_queue_tiocb(index->driver, &req->tiocb);

	return 0;
}

static int
vhd_index_queue_request(vhd_index_t *index, td_request_t treq)
{
	vhd_index_block_t *block;
	vhd_index_request_t *req;

	req = vhd_index_allocate_request(index);
	if (!req)
		return -EBUSY;

	req->treq = treq;

	block = vhd_index_get_block(index, treq.sec / index->vhdi.spb);
	ASSERT(block && td_flag_test(block->state, VHD_INDEX_BLOCK_READ_PENDING));

	list_add_tail(&req->next, &block->queue);
	return 0;
}

static void
vhd_index_queue_read(td_driver_t *driver, td_request_t treq)
{
	vhd_index_t *index;

	index = (vhd_index_t *)driver->data;

	while (treq.secs) {
		int err;
		td_request_t clone;

		err   = 0;
		clone = treq;

		switch (vhd_index_read_cache(index, clone.sec)) {
		case -EINVAL:
			err = -EINVAL;
			goto fail;

		case VHD_INDEX_BAT_CLEAR:
			clone.secs = MIN(clone.secs, index->vhdi.spb - (clone.sec % index->vhdi.spb));
			td_forward_request(clone);
			break;

		case VHD_INDEX_BIT_CLEAR:
			clone.secs = vhd_index_read_cache_span(index, clone.sec, clone.secs, 0);
			td_forward_request(clone);
			break;

		case VHD_INDEX_BIT_SET:
			clone.secs = vhd_index_read_cache_span(index, clone.sec, clone.secs, 1);
			err = vhd_index_schedule_data_read(index, clone);
			if (err)
				goto fail;
			break;

		case VHD_INDEX_CACHE_MISS:
			err = vhd_index_schedule_meta_read(index, clone.sec / index->vhdi.spb);
			if (err)
				goto fail;

			clone.secs = MIN(clone.secs, index->vhdi.spb - (clone.sec % index->vhdi.spb));
			vhd_index_queue_request(index, clone);
			break;

		case VHD_INDEX_META_READ_PENDING:
			clone.secs = MIN(clone.secs, index->vhdi.spb - (clone.sec % index->vhdi.spb));
			err = vhd_index_queue_request(index, clone);
			if (err)
				goto fail;
			break;
		}

		treq.sec  += clone.secs;
		treq.secs -= clone.secs;
		treq.buf  += vhd_sectors_to_bytes(clone.secs);
		continue;

	fail:
		clone.secs = treq.secs;
		td_complete_request(clone, err);
		break;
	}
}

static void
vhd_index_queue_write(td_driver_t *driver, td_request_t treq)
{
	td_complete_request(treq, -EPERM);
}

static inline void
vhd_index_signal_completion(vhd_index_t *index,
			    vhd_index_request_t *req, int err)
{
	td_complete_request(req->treq, err);
	vhd_index_put_file_ref(req->file);
	vhd_index_free_request(index, req);
}

static void
vhd_index_complete_meta_read(void *arg, struct tiocb *tiocb, int err)
{
	int i;
	uint32_t blk;
	td_request_t treq;
	vhd_index_t *index;
	vhd_index_block_t *block;
	vhd_index_request_t *req, *r, *tmp;

	req   = (vhd_index_request_t *)arg;
	index = req->index;

	blk   = req->treq.sec / index->vhdi.spb;
	block = vhd_index_get_block(index, blk);
	ASSERT(block && td_flag_test(block->state, VHD_INDEX_BLOCK_READ_PENDING));
	td_flag_clear(block->state, VHD_INDEX_BLOCK_READ_PENDING);

	if (err) {
		memset(block->vhdi_block.table, 0, block->table_size);
		vhd_index_block_for_each_request(block, r, tmp)
			vhd_index_signal_completion(index, r, err);
		return;
	}

	for (i = 0; i < block->vhdi_block.entries; i++)
		vhdi_entry_in(block->vhdi_block.table + i);

	td_flag_set(block->state, VHD_INDEX_BLOCK_VALID);

	vhd_index_block_for_each_request(block, r, tmp) {
		treq = r->treq;
		vhd_index_free_request(index, r);
		vhd_index_queue_read(index->driver, treq);
	}
}

static void
vhd_index_complete_data_read(void *arg, struct tiocb *tiocb, int err)
{
	vhd_index_t *index;
	vhd_index_request_t *req;

	req   = (vhd_index_request_t *)arg;
	index = req->index;

	vhd_index_signal_completion(index, req, err);
}

static int
vhd_index_get_parent_id(td_driver_t *driver, td_disk_id_t *id)
{
	return -EINVAL;
}

static int
vhd_index_validate_parent(td_driver_t *driver,
			  td_driver_t *parent, td_flag_t flags)
{
	return -EINVAL;
}

static void
vhd_index_debug(td_driver_t *driver)
{
	int i;
	vhd_index_t *index;

	index = (vhd_index_t *)driver->data;

	WARN("VHD INDEX %s\n", index->name);
	WARN("FILES:\n");
	for (i = 0; i < index->files.entries; i++) {
		int j, fd, refcnt;

		fd     = -1;
		refcnt = 0;

		for (j = 0; j < VHD_INDEX_FILE_POOL_SIZE; j++)
			if (index->fds[j].fid == index->files.table[i].file_id) {
				fd     = index->fds[j].fd;
				refcnt = index->fds[j].refcnt;
			}

		WARN("%s %u %d %d\n",
		     index->files.table[i].path,
		     index->files.table[i].file_id,
		     fd, refcnt);
	}

	WARN("REQUESTS:\n");
	for (i = 0; i < VHD_INDEX_REQUESTS; i++) {
		vhd_index_request_t *req;

		req = index->requests_list + i;

		if (!req->index)
			continue;

		WARN("%d: buf: %p, sec: 0x%08"PRIx64", secs: 0x%04x, "
		     "fid: %u, off: 0x%016"PRIx64"\n", i, req->treq.buf,
		     req->treq.sec, req->treq.secs, req->file->fid, req->off);
	}

	WARN("BLOCKS:\n");
	for (i = 0; i < VHD_INDEX_CACHE_SIZE; i++) {
		int queued;
		vhd_index_block_t *block;
		vhd_index_request_t *req, *tmp;

		queued = 0;
		block  = index->cache[i];

		if (!block)
			continue;

		vhd_index_block_for_each_request(block, req, tmp)
			++queued;

		WARN("%d: blk: 0x%08"PRIx64", state: 0x%08x, queued: %d\n",
		     i, block->blk, block->state, queued);
	}
}

struct tap_disk tapdisk_vhd_index = {
	.disk_type                = "tapdisk_vhd_index",
	.flags                    = 0,
	.private_data_size        = sizeof(vhd_index_t),
	.td_open                  = vhd_index_open,
	.td_close                 = vhd_index_close,
	.td_queue_read            = vhd_index_queue_read,
	.td_queue_write           = vhd_index_queue_write,
	.td_get_parent_id         = vhd_index_get_parent_id,
	.td_validate_parent       = vhd_index_validate_parent,
	.td_debug                 = vhd_index_debug,
};
