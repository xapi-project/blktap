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
