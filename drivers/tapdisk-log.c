/*
 * Copyright (c) 2008, 2009, XenSource Inc.
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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "blktaplib.h"
#include "tapdisk-log.h"
#include "tapdisk-utils.h"
#include "tapdisk-logfile.h"
#include "tapdisk-vbd-stats.h"

#define TLOG_LOGFILE_BUFSZ (16<<10)

#define MAX_ENTRY_LEN      512
#define MAX_ERROR_MESSAGES 16

struct error {
	int            cnt;
	int            err;
	char          *func;
	char           msg[MAX_ENTRY_LEN];
	struct dispersion st;
};

struct ehandle {
	int            cnt;
	int            dropped;
	struct error   errors[MAX_ERROR_MESSAGES];
};

struct tlog {
	char          *ident;
	td_logfile_t   logfile;
	int            precious;
	int            level;
};

static struct ehandle tapdisk_err;
static struct tlog tapdisk_log;

static void
tlog_logfile_vprint(const char *fmt, va_list ap)
{
	tapdisk_logfile_vprintf(&tapdisk_log.logfile, fmt, ap);
}

static void
__attribute__((format(printf, 1, 2)))
tlog_logfile_print(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	tlog_logfile_vprint(fmt, ap);
	va_end(ap);
}

static void
tlog_logfile_save(void)
{
	td_logfile_t *logfile = &tapdisk_log.logfile;
	const char *ident = tapdisk_log.ident;
	int err;

	tlog_logfile_print("%s: saving log, %d errors",
			   tapdisk_syslog_ident(ident), tapdisk_err.cnt);

	tapdisk_logfile_flush(logfile);

	err = tapdisk_logfile_rename(logfile,
				     TLOG_DIR, ident, ".log");
#if 0
	tlog_syslog("logfile saved to %s: %d\n", logfile->path, err);
#endif
}

static void
tlog_logfile_close(void)
{
	td_logfile_t *logfile = &tapdisk_log.logfile;
	const char *ident = tapdisk_log.ident;
	int keep, err;

	keep = tapdisk_log.precious || tapdisk_err.cnt;

	tlog_logfile_print("%s: closing log, %d errors",
			   tapdisk_syslog_ident(ident), tapdisk_err.cnt);

	if (keep) {
		tlog_logfile_save();
		DPRINTF("logfile kept as %s\n", logfile->path);
	}

	tapdisk_logfile_close(logfile);

	if (!keep)
		tapdisk_logfile_unlink(logfile);
}

static int
tlog_logfile_open(const char *ident, int level)
{
	td_logfile_t *logfile = &tapdisk_log.logfile;
	int mode, err;

	err = mkdir(TLOG_DIR, 0755);
	if (err) {
		err = -errno;
		if (err != -EEXIST)
			goto fail;
	}

	err = tapdisk_logfile_open(logfile,
				   TLOG_DIR, ident, ".tmp",
				   TLOG_LOGFILE_BUFSZ);
	if (err)
		goto fail;

	mode = (level == TLOG_DBG) ? _IOLBF : _IOFBF;

	err = tapdisk_logfile_setvbuf(logfile, mode);
	if (err)
		goto fail;

	tlog_logfile_print("%s: log start, level %d",
			   tapdisk_syslog_ident(ident), level);

	return 0;

fail:
	tlog_logfile_close();
	return err;
}

static void
tlog_logfile_error(int err, const char *func, const char *fmt, va_list ap)
{
	td_logfile_t *logfile = &tapdisk_log.logfile;
	char buf[MAX_ENTRY_LEN+1];

	vsnprintf(buf, sizeof(buf), fmt, ap);

	tlog_logfile_print("ERROR: errno %d at %s: %s", err, func, buf);

	tlog_precious();
}

static void
tlog_errors_init(const char *ident, int facility)
{
	int i;

	for (i = 0; i < MAX_ERROR_MESSAGES; ++i)
		td_dispersion_init(&tapdisk_err.errors[i].st);
}

int
tlog_open(const char *ident, int facility, int level)
{
	int err;

	DPRINTF("tlog starting, level %d\n", level);

	tapdisk_log.level = level;
	tapdisk_log.ident = strdup(ident);

	if (!tapdisk_log.ident) {
		err = -errno;
		goto fail;
	}

	err = tlog_logfile_open(ident, level);
	if (err)
		goto fail;

	tlog_errors_init(ident, facility);

	return 0;

fail:
	tlog_close();
	return err;
}

void
tlog_close(void)
{
	DPRINTF("tlog closing with %d errors\n", tapdisk_err.cnt);

	tlog_logfile_close();

	free(tapdisk_log.ident);
	tapdisk_log.ident = NULL;
}

void
tlog_precious(void)
{
	if (!tapdisk_log.precious)
		tlog_logfile_save();

	tapdisk_log.precious = 1;
}

void
__tlog_write(int prio, const char *fmt, ...)
{
	va_list ap;

	if (prio <= tapdisk_log.level) {
		va_start(ap, fmt);
		tlog_logfile_vprint(fmt, ap);
		va_end(ap);
	}
}

static void
__tlog_strtime(char *s, size_t len, struct timeval *tv)
{
	struct tm tm;
	char *buf;

	localtime_r(&tv->tv_sec, &tm);

	buf = s;
	buf += strftime(buf, len, "%b %d %H:%M:%S", &tm);
	buf += snprintf(buf, s + len - buf, ".%06lu", tv->tv_usec);
}

static void
tlog_strtime(char *tstr, size_t len)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	__tlog_strtime(tstr, len, &tv);
}

void
__tlog_error(int err, const char *func, const char *fmt, ...)
{
	va_list ap;
	int i, len, ret;
	struct error *e;

	err = (err > 0 ? err : -err);

	va_start(ap, fmt);
	tlog_logfile_error(err, func, fmt, ap);
	va_end(ap);

	for (i = 0; i < tapdisk_err.cnt; i++) {
		e = &tapdisk_err.errors[i];
		if (e->err == err && e->func == func) {
			e->cnt++;
			td_dispersion_add_now(&e->st);
			return;
		}
	}

	if (tapdisk_err.cnt >= MAX_ERROR_MESSAGES) {
		tapdisk_err.dropped++;
		return;
	}

	e = &tapdisk_err.errors[tapdisk_err.cnt];

	va_start(ap, fmt);
	ret = vsnprintf(e->msg, MAX_ENTRY_LEN, fmt, ap);
	va_end(ap);

	e->cnt++;
	e->err  = err;
	e->func = (char *)func;
	td_dispersion_add_now(&e->st);
	tapdisk_err.cnt++;
}

void
tlog_print_errors(void)
{
	int i;
	struct error *e;

	for (i = 0; i < tapdisk_err.cnt; i++) {
		char first[64], last[64];
		struct timeval tv;

		e = &tapdisk_err.errors[i];

		tv = TD_STATS_MIN(&e->st);
		__tlog_strtime(first, sizeof(first), &tv);

		tv = TD_STATS_MAX(&e->st);
		__tlog_strtime(last,  sizeof(last),  &tv);

		tv = TD_STATS_STDEV(&e->st);
		syslog(LOG_INFO, "TAPDISK ERROR: errno %d at %s (cnt = %d): "
		       "[%s/%s/%lu.%03lu] %s\n", e->err, e->func, e->cnt,
		       first, last, tv.tv_sec, tv.tv_usec / 1000, e->msg);
	}

	if (tapdisk_err.dropped)
		syslog(LOG_INFO, "TAPDISK ERROR: %d other error messages "
		       "dropped\n", tapdisk_err.dropped);
}

void
tapdisk_start_logging(const char *ident, const char *_facility)
{
	static char buf[128];
	int facility, err;

	facility = tapdisk_syslog_facility(_facility);

	snprintf(buf, sizeof(buf), "%s[%d]", ident, getpid());
	openlog(buf, LOG_CONS|LOG_ODELAY, facility);

	err = tlog_open(ident, facility, TLOG_WARN);
	if (err)
		EPRINTF("tlog open failure: %d\n", err);
}

void
tapdisk_stop_logging(void)
{
	tlog_close();
	closelog();
}
