/* Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */
#ifndef _LIB_VHDI_H_
#define _LIB_VHDI_H_

#include <inttypes.h>
#include <uuid/uuid.h>

#define VHD_MAX_NAME_LEN                    1024

typedef struct vhdi_context                 vhdi_context_t;
typedef struct vhdi_bat                     vhdi_bat_t;
typedef struct vhdi_block                   vhdi_block_t;
typedef struct vhdi_entry                   vhdi_entry_t;
typedef uint32_t                            vhdi_file_id_t;
typedef struct vhdi_file_ref                vhdi_file_ref_t;
typedef struct vhdi_file_table              vhdi_file_table_t;

struct vhdi_context {
	int                                 fd;
	int                                 spb;
	char                               *name;
	uint32_t                            vhd_block_size;
};

struct vhdi_bat {
	uint32_t                           *table;
	uint64_t                            vhd_blocks;
	uint32_t                            vhd_block_size;
	char                                vhd_path[VHD_MAX_NAME_LEN];
	char                                index_path[VHD_MAX_NAME_LEN];
	char                                file_table_path[VHD_MAX_NAME_LEN];
};

struct vhdi_entry {
	vhdi_file_id_t                      file_id;
	uint32_t                            offset;
};

struct vhdi_block {
	int                                 entries;
	vhdi_entry_t                       *table;
};

struct vhdi_file_ref {
	vhdi_file_id_t                      file_id;
	char                               *path;
	uuid_t                              vhd_uuid;
	uint32_t                            vhd_timestamp;
};

struct vhdi_file_table {
	int                                 entries;
	vhdi_file_ref_t                    *table;
};

void vhdi_entry_in(vhdi_entry_t *);

int vhdi_create(const char *, uint32_t);
int vhdi_open(vhdi_context_t *, const char *, int);
void vhdi_close(vhdi_context_t *);
int vhdi_read_block(vhdi_context_t *, vhdi_block_t *, uint32_t);
int vhdi_write_block(vhdi_context_t *, vhdi_block_t *, uint32_t);
int vhdi_append_block(vhdi_context_t *, vhdi_block_t *, uint32_t *);

int vhdi_bat_create(const char *, const char *, const char *, const char *);
int vhdi_bat_load(const char *, vhdi_bat_t *);
int vhdi_bat_write(const char *, vhdi_bat_t *);

int vhdi_file_table_create(const char *);
int vhdi_file_table_load(const char *, vhdi_file_table_t *);
int vhdi_file_table_add(const char *, const char *, vhdi_file_id_t *);
void vhdi_file_table_free(vhdi_file_table_t *);

#endif
