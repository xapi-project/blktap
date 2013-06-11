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
