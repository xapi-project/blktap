/* Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>

#include "tapdisk-log.h"
#include "tapdisk-vbd-stats.h"

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
	char          *p;
	int            size;
	uint64_t       cnt;
	char          *buf;
	int            level;
	char          *file;
	int            append;
};

static struct ehandle tapdisk_err;
static struct tlog tapdisk_log;

void
open_tlog(char *file, size_t bytes, int level, int append)
{
	int i;

	tapdisk_log.size = ((bytes + 511) & (~511));

	if (asprintf(&tapdisk_log.file, "%s.%d", file, getpid()) == -1)
		return;

	if (posix_memalign((void **)&tapdisk_log.buf, 512, tapdisk_log.size)) {
		free(tapdisk_log.file);
		tapdisk_log.buf = NULL;
		return;
	}

	memset(tapdisk_log.buf, 0, tapdisk_log.size);

	tapdisk_log.p      = tapdisk_log.buf;
	tapdisk_log.level  = level;
	tapdisk_log.append = append;

	for (i = 0; i < MAX_ERROR_MESSAGES; ++i)
		td_dispersion_init(&tapdisk_err.errors[i].st);
}

void
close_tlog(void)
{
	if (!tapdisk_log.buf)
		return;

	if (tapdisk_log.append)
		tlog_flush();

	free(tapdisk_log.buf);
	free(tapdisk_log.file);

	memset(&tapdisk_log, 0, sizeof(struct tlog));
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
__tlog_write(int level, const char *func, const char *fmt, ...)
{
	char *buf;
	va_list ap;
	int ret, len, avail;
	char tstr[64];

	if (!tapdisk_log.buf)
		return;

	if (level > tapdisk_log.level)
		return;

	avail = tapdisk_log.size - (tapdisk_log.p - tapdisk_log.buf);
	if (avail < MAX_ENTRY_LEN) {
		if (tapdisk_log.append)
			tlog_flush();
		tapdisk_log.p = tapdisk_log.buf;
	}

	buf = tapdisk_log.p;
	tlog_strtime(tstr, sizeof(tstr));
	len = snprintf(buf, MAX_ENTRY_LEN - 1, "%08"PRIu64":[%s]:%s ",
		       tapdisk_log.cnt, tstr, func);

	va_start(ap, fmt);
	ret = vsnprintf(buf + len, MAX_ENTRY_LEN - (len + 1), fmt, ap);
	va_end(ap);

	len = (ret < MAX_ENTRY_LEN - (len + 1) ?
	       len + ret : MAX_ENTRY_LEN - 1);
	buf[len] = '\0';

	tapdisk_log.cnt++;
	tapdisk_log.p += len;
}

void
__tlog_error(int err, const char *func, const char *fmt, ...)
{
	va_list ap;
	int i, len, ret;
	struct error *e;

	err = (err > 0 ? err : -err);

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
tlog_flush_errors(void)
{
	int i;
	struct error *e;

	for (i = 0; i < tapdisk_err.cnt; i++) {
		e = &tapdisk_err.errors[i];
		tlog_write(TLOG_WARN, "TAPDISK ERROR: errno %d at %s "
			   "(cnt = %d): %s\n", e->err, e->func, e->cnt,
			   e->msg);
	}

	if (tapdisk_err.dropped)
		tlog_write(TLOG_WARN, "TAPDISK ERROR: %d other error messages "
		       "dropped\n", tapdisk_err.dropped);
}

void
tlog_flush(void)
{
	int fd, flags;
	size_t size, wsize;

	if (!tapdisk_log.buf)
		return;

	flags = O_CREAT | O_WRONLY | O_DIRECT | O_NONBLOCK;
	if (!tapdisk_log.append)
		flags |= O_TRUNC;

	fd = open(tapdisk_log.file, flags, 0644);
	if (fd == -1)
		return;

	if (tapdisk_log.append)
		if (lseek64(fd, 0, SEEK_END) == (loff_t)-1)
			goto out;

	tlog_flush_errors();

	size  = tapdisk_log.p - tapdisk_log.buf;
	wsize = ((size + 511) & (~511));

	memset(tapdisk_log.buf + size, '\n', wsize - size);
	write(fd, tapdisk_log.buf, wsize);

	tapdisk_log.p = tapdisk_log.buf;

out:
	close(fd);
}
