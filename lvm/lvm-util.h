/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
