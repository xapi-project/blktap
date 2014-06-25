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

#ifndef _TAPDISK_UTILS_H_
#define _TAPDISK_UTILS_H_

#include <inttypes.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MAX_NAME_LEN          1000
#define TD_SYSLOG_IDENT_MAX   32
#define TD_SYSLOG_STRTIME_LEN 15

int tapdisk_syslog_facility(const char *);
char* tapdisk_syslog_ident(const char *);
size_t tapdisk_syslog_strftime(char *, size_t, const struct timeval *);
size_t tapdisk_syslog_strftv(char *, size_t, const struct timeval *);
int tapdisk_set_resource_limits(void);
int tapdisk_namedup(char **, const char *);
int tapdisk_parse_disk_type(const char *, char **, int *);
int tapdisk_get_image_size(int, uint64_t *, uint32_t *);
int tapdisk_linux_version(void);
uint64_t ntohll(uint64_t);
#define htonll ntohll


/**
 * Simplified version of snprintf that returns 0 if everything has gone OK and
 * +errno if not (including the buffer not being large enough to hold the
 * string).
 */
int
tapdisk_snprintf(char *buf, int * const off, int * const size,
		unsigned int depth,	const char *format, ...);

struct shm
{
    char *path;
    int fd;
    void *mem;
    unsigned size;
};

/**
 * Initialises a shm structure.
 */
void
shm_init(struct shm *shm);

/**
 * Creates the file in /dev/shm. The caller must populate the path and size
 * members of the shm structure passed to this function. Upon successful
 * completion of this function, the caller can use the shm->mem to write up to
 * shm.size bytes.
 *
 * Returns 0 in success, +errno on failure.
 *
 * XXX NB if the file is externally written to, the file size will change so
 * the caller must cope with it (e.g. manually call ftruncate(2)).
 */
int
shm_create(struct shm *shm);

/**
 * Destroys the file in /dev/shm. The caller is responsible for deallocating
 * the path member in struct shm.
 *
 * Returns 0 in success, +errno on failure.
 */
int
shm_destroy(struct shm *shm);

inline long long timeval_to_us(struct timeval *tv);

#endif
