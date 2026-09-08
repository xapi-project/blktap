/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TAPDISK_STORAGE_H_
#define _TAPDISK_STORAGE_H_

#define TAPDISK_STORAGE_TYPE_NFS       1
#define TAPDISK_STORAGE_TYPE_EXT       2
#define TAPDISK_STORAGE_TYPE_LVM       3

int tapdisk_storage_type(const char *path);
const char *tapdisk_storage_name(int type);

#endif
