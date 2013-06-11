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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/mman.h>

#include "tapdisk-logfile.h"
#include "tapdisk-utils.h"

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

static inline size_t
page_align(size_t size)
{
	size_t page_size = sysconf(_SC_PAGE_SIZE);
	return (size + page_size - 1) & ~(page_size - 1);
}

static void
tapdisk_logfile_free_buffer(td_logfile_t *log)
{
	if (log->vbuf) {
		munmap(log->vbuf, page_align(log->vbufsz));
		log->vbuf = NULL;
	}
}

static int
tapdisk_logfile_init_buffer(td_logfile_t *log, size_t size)
{
	int prot, flags, err;

	if (!size)
		return -EINVAL;

	prot  = PROT_READ|PROT_WRITE;
	flags = MAP_ANONYMOUS|MAP_PRIVATE;

	log->vbuf = mmap(NULL, page_align(size), prot, flags, -1, 0);
	if (log->vbuf == MAP_FAILED) {
		log->vbuf = NULL;
		goto fail;
	}

	err = mlock(log->vbuf, page_align(size));
	if (err)
		goto fail;

	log->vbufsz = size;

	return 0;

fail:
	tapdisk_logfile_free_buffer(log);
	err = -errno;
	return err;
}

int
tapdisk_logfile_unlink(td_logfile_t *log)
{
	int err;

	err = unlink(log->path);
	if (err)
		err = -errno;

	return err;
}

static int
__tapdisk_logfile_rename(td_logfile_t *log, const char *newpath)
{
	const size_t max = sizeof(log->path);
	int err;

	if (!strcmp(log->path, newpath))
		return 0;

	if (strlen(newpath) > max)
		return -ENAMETOOLONG;

	err = rename(log->path, newpath);
	if (err) {
		err = -errno;
		return err;
	}

	strncpy(log->path, newpath, max);

	return 0;
}

static int
tapdisk_logfile_name(char *path, size_t size,
		     const char *dir, const char *ident, const char *suffix)
{
	const size_t max = MIN(size, TD_LOGFILE_PATH_MAX);
	return snprintf(path, max, "%s/%s.%d%s", dir, ident, getpid(), suffix);
}

int
tapdisk_logfile_rename(td_logfile_t *log,
		       const char *dir, const char *ident, const char *suffix)
{
	char newpath[TD_LOGFILE_PATH_MAX+1];

	tapdisk_logfile_name(newpath, sizeof(newpath), dir, ident, suffix);

	return __tapdisk_logfile_rename(log, newpath);
}

void
tapdisk_logfile_close(td_logfile_t *log)
{
	if (log->file) {
		fclose(log->file);
		log->file = NULL;
	}

	tapdisk_logfile_free_buffer(log);
}

int
tapdisk_logfile_open(td_logfile_t *log,
		     const char *dir, const char *ident, const char *ext,
		     size_t bufsz)
{
	int err;

	memset(log, 0, sizeof(log));

	tapdisk_logfile_name(log->path, sizeof(log->path), dir, ident, ext);

	log->file = fopen(log->path, "w");
	if (!log->file) {
		err = -errno;
		goto fail;
	}

	err = tapdisk_logfile_init_buffer(log, bufsz);
	if (err)
		goto fail;

	return 0;

fail:
	tapdisk_logfile_unlink(log);
	tapdisk_logfile_close(log);
	return err;
}

int
tapdisk_logfile_setvbuf(td_logfile_t *log, int mode)
{
	int err = 0;

	if (log->file) {
		err = setvbuf(log->file, log->vbuf, mode, log->vbufsz);
		if (err)
			err = -errno;
	}

	return err;
}

ssize_t
tapdisk_logfile_vprintf(td_logfile_t *log, const char *fmt, va_list ap)
{
	char buf[1024];
	size_t size, n;
	ssize_t len;
	struct timeval tv;

	if (!log->file)
		return -EBADF;

	gettimeofday(&tv, NULL);

	size = sizeof(buf);
	len  = 0;

	len += tapdisk_syslog_strftime(buf, size, &tv);
	len += snprintf(buf + len, size - len, ": ");
	len += tapdisk_syslog_strftv(buf + len, size - len, &tv);
	len += snprintf(buf + len, size - len, " ");
	len += vsnprintf(buf + len, size - len, fmt, ap);

	if (buf[len-1] != '\n')
		len += snprintf(buf + len, size - len, "\n");

	n = fwrite(buf, len, 1, log->file);
	if (n != len)
		len = -ferror(log->file);

	return len;
}

ssize_t
tapdisk_logfile_printf(td_logfile_t *log, const char *fmt, ...)
{
	va_list ap;
	int rv;

	va_start(ap, fmt);
	rv = tapdisk_logfile_vprintf(log, fmt, ap);
	va_end(ap);

	return rv;
}

int
tapdisk_logfile_flush(td_logfile_t *log)
{
	int rv = EOF;

	if (log->file)
		rv = fflush(log->file);

	return rv;
}
