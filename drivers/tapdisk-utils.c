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
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <linux/version.h>
#endif

#define SYSLOG_NAMES
#include <syslog.h>
#include <stdarg.h>

#include "tapdisk.h"
#include "tapdisk-log.h"
#include "tapdisk-utils.h"
#include "tapdisk-syslog.h"

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

#define ASSERT(_p)										\
	if (!(_p)) {											\
		EPRINTF("%s:%d: FAILED ASSERTION: '%s'\n",			\
			__FILE__, __LINE__, #_p);						\
		td_panic();											\
	}

static int
tapdisk_syslog_facility_by_name(const char *name)
{
	int facility;
	CODE *c;

	facility = -1;

	for (c = facilitynames; c->c_name != NULL; ++c)
		if (!strcmp(c->c_name, name)) {
			facility = c->c_val;
			break;
		}

	return facility;
}

int
tapdisk_syslog_facility(const char *arg)
{
	int facility;
	char *endptr;

	if (arg) {
		facility = strtol(arg, &endptr, 0);
		if (*endptr == 0)
			return facility;

		facility = tapdisk_syslog_facility_by_name(arg);
		if (facility >= 0)
			return facility;
	}

	return LOG_DAEMON;
}

char*
tapdisk_syslog_ident(const char *name)
{
	char ident[TD_SYSLOG_IDENT_MAX+1];
	size_t size, len;
	pid_t pid;

	pid  = getpid();
	size = sizeof(ident);
	len  = 0;

	len  = snprintf(NULL, 0, "[%d]", pid);
	len  = snprintf(ident, size - len, "%s", name);
	len += snprintf(ident + len, size - len, "[%d]", pid);

	return strdup(ident);
}

size_t
tapdisk_syslog_strftime(char *buf, size_t size, const struct timeval *tv)
{
	const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
			      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	struct tm tm;

	/*
	 * TIMESTAMP :=  <Mmm> " " <dd> " " <hh> ":" <mm> ":" <ss>.
	 * Local time, no locales.
	 */

	localtime_r(&tv->tv_sec, &tm);

	return snprintf(buf, size, "%s %2d %02d:%02d:%02d",
			mon[tm.tm_mon], tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
}

size_t
tapdisk_syslog_strftv(char *buf, size_t size, const struct timeval *tv)
{
	struct tm tm;

	localtime_r(&tv->tv_sec, &tm);

	return snprintf(buf, size, "[%02d:%02d:%02d.%03ld]",
			tm.tm_hour, tm.tm_min, tm.tm_sec,
			(long)tv->tv_usec / 1000);
}

int
tapdisk_set_resource_limits(void)
{
	int err;
	struct rlimit rlim;

	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;

	err = setrlimit(RLIMIT_MEMLOCK, &rlim);
	if (err == -1) {
		EPRINTF("RLIMIT_MEMLOCK failed: %d\n", errno);
		return -errno;
	}

	err = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (err == -1) {
		EPRINTF("mlockall failed: %d\n", errno);
		return -errno;
	}

#define CORE_DUMP
#if defined(CORE_DUMP)
	err = setrlimit(RLIMIT_CORE, &rlim);
	if (err == -1)
		EPRINTF("RLIMIT_CORE failed: %d\n", errno);
#endif

	return 0;
}

int
tapdisk_namedup(char **dup, const char *name)
{
	*dup = NULL;

	if (strnlen(name, MAX_NAME_LEN) >= MAX_NAME_LEN)
		return -ENAMETOOLONG;
	
	*dup = strdup(name);
	if (!*dup)
		return -ENOMEM;

	return 0;
}

/*Get Image size, secsize*/
int
tapdisk_get_image_size(int fd, uint64_t *_sectors, uint32_t *_sector_size)
{
	struct stat stat;
	uint64_t sectors, bytes;
	uint32_t sector_size;

	sectors       = 0;
	sector_size   = 0;
	*_sectors     = 0;
	*_sector_size = 0;

	if (fstat(fd, &stat)) {
		DPRINTF("ERROR: fstat failed, Couldn't stat image");
		return -EINVAL;
	}

	if (S_ISBLK(stat.st_mode)) {
		/*Accessing block device directly*/
		if (ioctl(fd,BLKGETSIZE64,&bytes)==0) {
			sectors = bytes >> SECTOR_SHIFT;
		} else if (ioctl(fd,BLKGETSIZE,&sectors)!=0) {
			DPRINTF("ERR: BLKGETSIZE and BLKGETSIZE64 failed, couldn't stat image");
			return -EINVAL;
		}

		/*Get the sector size*/
#if defined(BLKSSZGET)
		{
			sector_size = DEFAULT_SECTOR_SIZE;
			ioctl(fd, BLKSSZGET, &sector_size);

			if (sector_size != DEFAULT_SECTOR_SIZE)
				DPRINTF("Note: sector size is %u (not %d)\n",
					sector_size, DEFAULT_SECTOR_SIZE);
		}
#else
		sector_size = DEFAULT_SECTOR_SIZE;
#endif

	} else {
		/*Local file? try fstat instead*/
		sectors     = (stat.st_size >> SECTOR_SHIFT);
		sector_size = DEFAULT_SECTOR_SIZE;
	}

	if (sectors == 0) {		
		sectors     = 16836057ULL;
		sector_size = DEFAULT_SECTOR_SIZE;
	}

	return 0;
}

#ifdef __linux__

int tapdisk_linux_version(void)
{
	struct utsname uts;
	unsigned int version, patchlevel, sublevel;
	int n, err;

	err = uname(&uts);
	if (err)
		return -errno;

	n = sscanf(uts.release, "%u.%u.%u", &version, &patchlevel, &sublevel);
        if (n != 3) {
                sublevel = 0;
                n = sscanf(uts.release, "%u.%u", &version, &patchlevel);
                if (n != 2)
                        return -ENOSYS;
        }

	return KERNEL_VERSION(version, patchlevel, sublevel);
}

#else

int tapdisk_linux_version(void)
{
	return -ENOSYS;
}

#endif

#ifdef WORDS_BIGENDIAN
uint64_t ntohll(uint64_t a) {
	return a;
}
#else
uint64_t ntohll(uint64_t a) {
	uint32_t lo = a & 0xffffffff;
	uint32_t hi = a >> 32U;
	lo = ntohl(lo);
	hi = ntohl(hi);
	return ((uint64_t) lo) << 32U | hi;
}
#endif
#define htonll ntohll


/**
 * Simplified version of snprintf that return 0 if everything has gone OK and
 * +errno if not (including the buffer not being large enough to hold the
 * string).
 */
int
tapdisk_snprintf(char *buf, int * const off, int * const size,
		unsigned int depth,	const char *format, ...) {

	int err, i;
	va_list ap;

	ASSERT(buf);
	ASSERT(off);
	ASSERT(size);

	for (i = 0; i < depth; i++) {
		err = snprintf(buf + *off, *size, "  ");
		if (err < 0)
			return errno;
		*off += err;
		*size -= err;
	}

	va_start(ap, format);
	err = vsnprintf(buf + *off, *size, format, ap);
	va_end(ap);
	if (err >= *size)
		return ENOBUFS;
	else if (err < 0)
		return errno;
	*off += err;
	*size -= err;
	return 0;
}
