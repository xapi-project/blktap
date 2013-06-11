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

#ifndef _TAPDISK_STORAGE_H_
#define _TAPDISK_STORAGE_H_

#define TAPDISK_STORAGE_TYPE_NFS       1
#define TAPDISK_STORAGE_TYPE_EXT       2
#define TAPDISK_STORAGE_TYPE_LVM       3

int tapdisk_storage_type(const char *path);
const char *tapdisk_storage_name(int type);

#endif
