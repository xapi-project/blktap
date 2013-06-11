/* 
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef _LVM_UTIL_H_
#define _LVM_UTIL_H_

#include <inttypes.h>

#define MAX_NAME_SIZE            256

#define LVM_SEG_TYPE_LINEAR      1
#define LVM_SEG_TYPE_UNKNOWN     2

struct lv_segment {
	uint8_t                  type;
	char                     device[MAX_NAME_SIZE];
	uint64_t                 pe_start;
	uint64_t                 pe_size;
};

struct lv {
	char                     name[MAX_NAME_SIZE];
	uint64_t                 size;
	uint32_t                 segments;
	struct lv_segment        first_segment;
};

struct pv {
	char                     name[MAX_NAME_SIZE];
	uint64_t                 start;
};

struct vg {
	char                     name[MAX_NAME_SIZE];
	uint64_t                 extent_size;

	int                      pv_cnt;
	struct pv               *pvs;

	int                      lv_cnt;
	struct lv               *lvs;
};

int lvm_scan_vg(const char *vg_name, struct vg *vg);
void lvm_free_vg(struct vg *vg);

#endif
