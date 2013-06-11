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

#ifndef _TAPDISK_UTILS_H_
#define _TAPDISK_UTILS_H_

#include <inttypes.h>
#include <sys/time.h>

#define MAX_NAME_LEN          1000
#define TD_SYSLOG_IDENT_MAX   32
#define TD_SYSLOG_STRTIME_LEN 15

int tapdisk_syslog_facility(const char *);
char* tapdisk_syslog_ident(const char *);
size_t tapdisk_syslog_strftime(char *, size_t, const struct timeval *);
size_t tapdisk_syslog_strftv(char *, size_t, const struct timeval *);
int tapdisk_set_resource_limits(void);
int tapdisk_namedup(char **, const char *);
int tapdisk_parse_disk_type(const char *, char **, int *);
int tapdisk_get_image_size(int, uint64_t *, uint32_t *);
int tapdisk_linux_version(void);
uint64_t ntohll(uint64_t);
#define htonll ntohll

#endif
