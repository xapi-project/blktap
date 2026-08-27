/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VHD_UTIL_H_
#define _VHD_UTIL_H_

int vhd_util_create(int argc, char **argv);
int vhd_util_snapshot(int argc, char **argv);
int vhd_util_query(int argc, char **argv);
int vhd_util_read(int argc, char **argv);
int vhd_util_set_field(int argc, char **argv);
int vhd_util_repair(int argc, char **argv);
int vhd_util_fill(int argc, char **argv);
int vhd_util_resize(int argc, char **argv);
int vhd_util_coalesce(int argc, char **argv);
int vhd_util_modify(int argc, char **argv);
int vhd_util_scan(int argc, char **argv);
int vhd_util_check(int argc, char **argv);
int vhd_util_revert(int argc, char **argv);
int vhd_util_key(int argc, char **argv);
int vhd_util_copy(int argc, char **argv);

#endif
