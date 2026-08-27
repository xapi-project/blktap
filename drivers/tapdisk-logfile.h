/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

int tapdisk_logfile_open(td_logfile_t *, const char *dir, const char *ident,
        size_t bufsz);

ssize_t tapdisk_logfile_printf(td_logfile_t *, const char *fmt, ...);
ssize_t tapdisk_logfile_vprintf(td_logfile_t *, const char *fmt, va_list ap);

void tapdisk_logfile_close(td_logfile_t *);
int tapdisk_logfile_unlink(td_logfile_t *);

int tapdisk_logfile_setvbuf(td_logfile_t *log, int mode);
int tapdisk_logfile_flush(td_logfile_t *);

#endif /* __TAPDISK_LOGFILE_H__ */
