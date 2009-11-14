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

#ifndef __TAPDISK_LOGFILE_H__
#define __TAPDISK_LOGFILE_H__

#include <stdio.h>

typedef struct _td_logfile td_logfile_t;

#define TD_LOGFILE_PATH_MAX    128UL

struct _td_logfile {
	char           path[TD_LOGFILE_PATH_MAX];
	FILE          *file;
	char          *vbuf;
	size_t         vbufsz;
};

int tapdisk_logfile_open(td_logfile_t *,
			 const char *dir, const char *ident, const char *ext,
			 size_t bufsz);

ssize_t tapdisk_logfile_printf(td_logfile_t *, const char *fmt, ...);
ssize_t tapdisk_logfile_vprintf(td_logfile_t *, const char *fmt, va_list ap);

void tapdisk_logfile_close(td_logfile_t *);
int tapdisk_logfile_unlink(td_logfile_t *);
int tapdisk_logfile_rename(td_logfile_t *,
			   const char *dir, const char *ident, const char *ext);

int tapdisk_logfile_setvbuf(td_logfile_t *log, int mode);
int tapdisk_logfile_flush(td_logfile_t *);

#endif /* __TAPDISK_LOGFILE_H__ */
