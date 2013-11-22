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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>

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
	int            facility;
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

/**
 * Closes the log file.
 *
 * @param keep if set to true the log file is never removed. NB if an error has
 * occurred or a USR1 has been received the log is always kept, independently
 * of whether @keep is set to false.
 */
static void
tlog_logfile_close(bool keep)
{
	td_logfile_t *logfile = &tapdisk_log.logfile;

	if (tapdisk_log.precious || tapdisk_log.errors)
		keep = true;

	tlog_info("closing log, %lu errors", tapdisk_log.errors);

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

	err = tapdisk_logfile_open(logfile, TLOG_DIR, name, TLOG_LOGFILE_BUFSZ);
	if (err)
		goto fail;

	mode = (level == TLOG_DBG) ? _IOLBF : _IOFBF;

	err = tapdisk_logfile_setvbuf(logfile, mode);
	if (err)
		goto fail;

	tlog_info("log start, level %d", level);

	return 0;

fail:
	tlog_logfile_close(false);
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
    static const int tlog_to_syslog[3] = {LOG_WARNING, LOG_INFO, LOG_DEBUG};

    prio = prio >= 0 && prio < 3 ? tlog_to_syslog[prio] : LOG_INFO;

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
	tapdisk_log.facility = facility;

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

int
tlog_reopen(void)
{
	int err;

	tlog_logfile_close(true);
	err = tlog_logfile_open(tapdisk_log.name, tapdisk_log.level);
	if (err)
		return err;

	tlog_syslog_close();
	return tlog_syslog_open(tapdisk_log.ident, tapdisk_log.facility);
}

void
tlog_close(void)
{
	DPRINTF("tapdisk-log: closing after %lu errors\n",
		tapdisk_log.errors);

	tlog_logfile_close(false);
	tlog_syslog_close();

	free(tapdisk_log.ident);
	tapdisk_log.ident = NULL;
}

void
tlog_precious(int force_flush)
{
	if (!tapdisk_log.precious || force_flush)
		tapdisk_logfile_flush(&tapdisk_log.logfile);

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
	int facility;

	facility = tapdisk_syslog_facility(_facility);
	tapdisk_server_openlog(ident, LOG_CONS|LOG_ODELAY, facility);
}

void
tapdisk_stop_logging(void)
{
	tapdisk_server_closelog();
}
