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

#include "tapdisk-log.h"
#include "tapdisk-utils.h"
#include "tapdisk-logfile.h"
#include "tapdisk-syslog.h"
#include "tapdisk-server.h"

#define TLOG_LOGFILE_BUFSZ (16<<10)
#define TLOG_SYSLOG_BUFSZ   (8<<10)

#define MAX_ENTRY_LEN      512

struct tlog {
	char          *name;
	td_logfile_t   logfile;
	int            precious;
	int            level;

	char          *ident;
	td_syslog_t    syslog;
	unsigned long  errors;
};

static struct tlog tapdisk_log;

static void
tlog_logfile_vprint(const char *fmt, va_list ap)
{
	tapdisk_logfile_vprintf(&tapdisk_log.logfile, fmt, ap);
}

static void __printf(1, 2)
tlog_logfile_print(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	tlog_logfile_vprint(fmt, ap);
	va_end(ap);
}

#define tlog_info(_fmt, _args ...)					\
	tlog_logfile_print("%s: "_fmt, tapdisk_log.ident, ##_args)

static void
tlog_logfile_save(void)
{
	td_logfile_t *logfile = &tapdisk_log.logfile;
	const char *name = tapdisk_log.name;
	int err;

	tlog_info("saving log, %lu errors", tapdisk_log.errors);

	tapdisk_logfile_flush(logfile);

	err = tapdisk_logfile_rename(logfile,
				     TLOG_DIR, name, ".log");

	tlog_syslog(LOG_INFO,
		    "logfile saved to %s: %d\n", logfile->path, err);
}

static void
tlog_logfile_close(void)
{
	td_logfile_t *logfile = &tapdisk_log.logfile;
	int keep, err;

	keep = tapdisk_log.precious || tapdisk_log.errors;

	tlog_info("closing log, %lu errors", tapdisk_log.errors);

	if (keep)
		tlog_logfile_save();

	tapdisk_logfile_close(logfile);

	if (!keep)
		tapdisk_logfile_unlink(logfile);
}

static int
tlog_logfile_open(const char *name, int level)
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
				   TLOG_DIR, name, ".tmp",
				   TLOG_LOGFILE_BUFSZ);
	if (err)
		goto fail;

	mode = (level == TLOG_DBG) ? _IOLBF : _IOFBF;

	err = tapdisk_logfile_setvbuf(logfile, mode);
	if (err)
		goto fail;

	tlog_info("log start, level %d", level);

	return 0;

fail:
	tlog_logfile_close();
	return err;
}

static void
tlog_syslog_close(void)
{
	td_syslog_t *syslog = &tapdisk_log.syslog;

	tapdisk_syslog_stats(syslog, LOG_INFO);
	tapdisk_syslog_flush(syslog);
	tapdisk_syslog_close(syslog);
}

static int
tlog_syslog_open(const char *ident, int facility)
{
	td_syslog_t *syslog = &tapdisk_log.syslog;
	int err;

	err = tapdisk_syslog_open(syslog,
				  tapdisk_log.ident, facility,
				  TLOG_SYSLOG_BUFSZ);
	return err;
}

void
tlog_vsyslog(int prio, const char *fmt, va_list ap)
{
	td_syslog_t *syslog = &tapdisk_log.syslog;

	tapdisk_vsyslog(syslog, prio, fmt, ap);
}

void
tlog_syslog(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	tlog_vsyslog(prio, fmt, ap);
	va_end(ap);
}

int
tlog_open(const char *name, int facility, int level)
{
	int err;

	DPRINTF("tapdisk-log: started, level %d\n", level);

	tapdisk_log.level = level;
	tapdisk_log.name  = strdup(name);
	tapdisk_log.ident = tapdisk_syslog_ident(name);

	if (!tapdisk_log.name || !tapdisk_log.ident) {
		err = -errno;
		goto fail;
	}

	err = tlog_logfile_open(tapdisk_log.name, level);
	if (err)
		goto fail;

	err = tlog_syslog_open(tapdisk_log.ident, facility);
	if (err)
		goto fail;

	return 0;

fail:
	tlog_close();
	return err;
}

void
tlog_close(void)
{
	DPRINTF("tapdisk-log: closing after %lu errors\n",
		tapdisk_log.errors);

	tlog_logfile_close();
	tlog_syslog_close();

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
__tlog_write(int level, const char *fmt, ...)
{
	va_list ap;

	if (level <= tapdisk_log.level) {
		va_start(ap, fmt);
		tlog_logfile_vprint(fmt, ap);
		va_end(ap);
	}
}

void
__tlog_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	tlog_vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);

	tapdisk_log.errors++;
}

void
tapdisk_start_logging(const char *ident, const char *_facility)
{
	static char buf[128];
	int facility, err;

	facility = tapdisk_syslog_facility(_facility);
	tapdisk_server_openlog(ident, LOG_CONS|LOG_ODELAY, facility);
}

void
tapdisk_stop_logging(void)
{
	tapdisk_server_closelog();
}
