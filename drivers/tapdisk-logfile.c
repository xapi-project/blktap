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
