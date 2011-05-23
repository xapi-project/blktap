/*
 * Copyright (c) 2009, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
void tlog_precious(void);
void tlog_vsyslog(int, const char *, va_list);
void tlog_syslog(int, const char *, ...) __printf(2, 3);

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
		     _err, __func__, ##_a)

#define tlog_drv_error(_drv, _err, _f, _a ...) do {	\
	if (tapdisk_driver_log_pass(_drv, __func__))	\
		tlog_error(_err, _f, ##_a);		\
} while (0)

#endif
