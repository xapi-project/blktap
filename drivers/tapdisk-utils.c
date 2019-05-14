/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
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
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
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

void
shm_init(struct shm *shm) {

    ASSERT(shm);

    shm->path = NULL;
    shm->fd = -1;
    shm->mem = NULL;
    shm->size = 0;
}


int
shm_destroy(struct shm *shm) {

    int err = 0;

    ASSERT(shm);

    if (shm->mem) {
        err = munmap(shm->mem, shm->size);
        if (err == -1) {
            err = errno;
            EPRINTF("failed to munmap %s: %s\n", shm->path,
                    strerror(err));
            goto out;
        }
        shm->mem = NULL;
    }

    if (shm->fd != -1) {
        do {
            err = close(shm->fd);
            if (err)
                err = errno;
        } while (err == EINTR);
        if (err) {
            EPRINTF("failed to close %s: %s\n", shm->path, strerror(err));
            goto out;
        }
        shm->fd = -1;
    }

    if (shm->path) {
        err = unlink(shm->path);
        if (unlikely(err == -1)) {
            err = errno;
            if (unlikely(err != ENOENT))
                goto out;
            else
                err = 0;
        }
    }

out:
    return err;
}


int
shm_create(struct shm *shm) {

    int err;

    ASSERT(shm);
    ASSERT(shm->path);
    ASSERT(shm->size);

    shm->fd = open(shm->path, O_CREAT | O_TRUNC | O_RDWR | O_EXCL,
            S_IRUSR | S_IWUSR);
    if (shm->fd == -1) {
        err = errno;
        EPRINTF("failed to open %s: %s\n", shm->path, strerror(err));
        goto out;
    }

    err = ftruncate(shm->fd, shm->size);
    if (err == -1) {
        err = errno;
        EPRINTF("failed to truncate %s: %s\n", shm->path, strerror(err));
        goto out;
    }

    shm->mem = mmap(NULL, shm->size, PROT_READ | PROT_WRITE, MAP_SHARED,
            shm->fd, 0);
    if (shm->mem == MAP_FAILED) {
        err = errno;
        EPRINTF("failed to mmap %s: %s\n", shm->path, strerror(err));
        goto out;
    }

out:
    if (err) {
        int err2 = shm_destroy(shm);
        if (err2)
            EPRINTF("failed to clean up failed shared memory file creation: "
                    "%s (error ignored)\n", strerror(-err2));
    }
    return err;
}

const long long USEC_PER_SEC = 1000000L;

inline long long timeval_to_us(struct timeval *tv)
{
	return ((long long)tv->tv_sec * USEC_PER_SEC) + tv->tv_usec;
}

bool is_hole_punching_supported_for_fd(int fd) {
        int rc;
        int kernel_version;
        struct statfs statfs_buf;

        rc = fstatfs(fd, &statfs_buf);
        if (rc)
                return false;
        kernel_version = tapdisk_linux_version();
        if (-ENOSYS == kernel_version)
                return false;

        // Support matrix according to man fallocate(2)
        switch (statfs_buf.f_type) {
#ifdef BTRFS_SUPER_MAGIC
                case BTRFS_SUPER_MAGIC:
                        return (kernel_version >= KERNEL_VERSION(3, 7, 0));
#endif
#ifdef EXT4_SUPER_MAGIC
                case EXT4_SUPER_MAGIC:
                        return (kernel_version >= KERNEL_VERSION(3, 0, 0));
#endif
#ifdef TMPFS_SUPER_MAGIC
                case TMPFS_SUPER_MAGIC:
                        return (kernel_version >= KERNEL_VERSION(3, 5, 0));
#endif
#ifdef XFS_SUPER_MAGIC
                case XFS_SUPER_MAGIC:
                        return (kernel_version >= KERNEL_VERSION(2, 6, 38));
#endif
                default:
                        break;
        }
        return false;
}
