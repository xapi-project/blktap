/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _LVM_UTIL_PRIV_H
#define _LVM_UTIL_PRIV_H
#include "lvm-util.h"

int
lvm_scan_vg(const char *vg_name, struct vg *vg);

void
lvm_free_vg(struct vg *vg);

#endif /*_LVM_UTIL_PRIV_H*/
