/* Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */
#ifndef _VHD_JOURNAL_H_
#define _VHD_JOURNAL_H_

#include <inttypes.h>

#include "libvhd.h"

#define VHD_JOURNAL_METADATA       0x01
#define VHD_JOURNAL_DATA           0x02

#define VHD_JOURNAL_HEADER_COOKIE  "vjournal"
#define VHD_JOURNAL_ENTRY_COOKIE   0xaaaa12344321aaaaULL

typedef struct vhd_journal_header {
	char                       cookie[8];
	uuid_t                     uuid;
	uint64_t                   vhd_footer_offset;
	uint32_t                   journal_data_entries;
	uint32_t                   journal_metadata_entries;
	uint64_t                   journal_data_offset;
	uint64_t                   journal_metadata_offset;
	uint64_t                   journal_eof;
	char                       pad[448];
} vhd_journal_header_t;

typedef struct vhd_journal {
	char                      *jname;
	int                        jfd;
	int                        is_block; /* is jfd a block device */
	vhd_journal_header_t       header;
	vhd_context_t              vhd;
} vhd_journal_t;

int vhd_journal_create(vhd_journal_t *, const char *file, const char *jfile);
int vhd_journal_open(vhd_journal_t *, const char *file, const char *jfile);
int vhd_journal_add_block(vhd_journal_t *, uint32_t block, char mode);
int vhd_journal_commit(vhd_journal_t *);
int vhd_journal_revert(vhd_journal_t *);
int vhd_journal_close(vhd_journal_t *);
int vhd_journal_remove(vhd_journal_t *);

#endif
