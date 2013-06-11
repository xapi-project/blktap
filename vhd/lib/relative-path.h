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

#ifndef _RELATIVE_PATH_H_
#define _RELATIVE_PATH_H_

#include <syslog.h>

#define DELIMITER    '/'
#define MAX_NAME_LEN 1000

#define EPRINTF(_f, _a...) syslog(LOG_ERR, "tap-err:%s: " _f, __func__, ##_a)

/*
 * returns a relative path from @src to @dest
 * result should be freed
 */
char *relative_path_to(char *src, char *dest, int *err);

#endif
