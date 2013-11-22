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

#ifndef _TAPDISK_LOG_H_
#define _TAPDISK_LOG_H_

#define TLOG_WARN       0
#define TLOG_INFO       1
#define TLOG_DBG        2

#define TLOG_DIR "/var/log/blktap"

#include <stdarg.h>
#include "compiler.h"

int  tlog_open(const char *, int, int);
void tlog_close(void);
void tlog_precious(int);
void tlog_vsyslog(int, const char *, va_list);
void tlog_syslog(int, const char *, ...) __printf(2, 3);
int tlog_reopen(void);

#include <syslog.h>

#define EPRINTF(_f, _a...) syslog(LOG_ERR, "tap-err:%s: " _f, __func__, ##_a)
#define DPRINTF(_f, _a...) syslog(LOG_INFO, _f, ##_a)
#define PERROR(_f, _a...)  EPRINTF(_f ": %s", ##_a, strerror(errno))

void __tlog_write(int, const char *, ...) __printf(2, 3);
void __tlog_error(const char *fmt, ...) __printf(1, 2);

#define tlog_write(_level, _f, _a...)			\
	__tlog_write(_level, "%s: " _f,  __func__, ##_a)

#define tlog_error(_err, _f, _a...)			\
	__tlog_error("ERROR: errno %d at %s: " _f,	\
		     (int)_err, __func__, ##_a)

#define tlog_drv_error(_drv, _err, _f, _a ...) do {	\
	if (tapdisk_driver_log_pass(_drv, __func__))	\
		tlog_error(_err, _f, ##_a);		\
} while (0)

#endif
