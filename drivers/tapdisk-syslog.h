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

#ifndef __TAPDISK_SYSLOG_H__
#define __TAPDISK_SYSLOG_H__

#include <syslog.h>
#include <stdarg.h>
#include "scheduler.h"

typedef struct _td_syslog td_syslog_t;

#define TD_SYSLOG_PACKET_MAX  1024

struct _td_syslog_stats {
	unsigned long long count;
	unsigned long long bytes;
	unsigned long long xmits;
	unsigned long long fails;
	unsigned long long drops;
};

struct _td_syslog {
	char            *ident;
	int              facility;

	int              sock;
	event_id_t       event_id;

	void            *buf;
	size_t           bufsz;

	char            *msg;

	char            *ring;
	size_t           ringsz;

	size_t           prod;
	size_t           cons;

	int              oom;
	struct timeval   oom_tv;

	struct _td_syslog_stats stats;
};

int  tapdisk_syslog_open(td_syslog_t *,
			 const char *ident, int facility, size_t bufsz);
void tapdisk_syslog_close(td_syslog_t *);
void tapdisk_syslog_flush(td_syslog_t *);
void tapdisk_syslog_stats(td_syslog_t *, int prio);

int tapdisk_vsyslog(td_syslog_t *, int prio, const char *fmt, va_list ap);
int tapdisk_syslog(td_syslog_t *, int prio, const char *fmt, ...);

#endif /* __TAPDISK_SYSLOG_H__ */
