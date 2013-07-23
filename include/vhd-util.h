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
int vhd_util_copy(const int argc, char **argv);
#endif
