/* 
 * API for writelog communication
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

#ifndef __LOG_H__
#define __LOG_H__ 1

#include <inttypes.h>

#define LOGCMD_SHMP  "shmp"
#define LOGCMD_PEEK  "peek"
#define LOGCMD_CLEAR "clrw"
#define LOGCMD_GET   "getw"
#define LOGCMD_KICK  "kick"

#define CTLRSPLEN_SHMP  256
#define CTLRSPLEN_PEEK  4
#define CTLRSPLEN_CLEAR 4
#define CTLRSPLEN_GET   4
#define CTLRSPLEN_KICK  0

/* shmregion is arbitrarily capped at 8 megs for a minimum of
 * 64 MB of data per read (if there are no contiguous regions)
 * In the off-chance that there is more dirty data, multiple
 * reads must be done */
#define SHMSIZE (8 * 1024 * 1024)
#define SRINGSIZE 4096

/* The shared memory region is split up into 3 subregions:
 * The first half is reserved for the dirty bitmap log.
 * The second half begins with 1 page for read request descriptors,
 * followed by a big area for supplying read data.
 */
static inline void* bmstart(void* shm)
{
  return shm;
}

static inline void* bmend(void* shm)
{
  return shm + SHMSIZE/2;
}

static inline void* sringstart(void* shm)
{
  return bmend(shm);
}

static inline void* sdatastart(void* shm)
{
  return sringstart(shm) + SRINGSIZE;
}

static inline void* sdataend(void* shm)
{
  return shm + SHMSIZE;
}

/* format for messages between log client and server */
struct log_ctlmsg {
  char msg[4];
  char params[16];
};

/* extent descriptor */
struct disk_range {
  uint64_t sector;
  uint32_t count;
};

/* dirty write logging space. This is an extent ring at the front,
 * full of disk_ranges plus a pointer into the data area */
/* I think I'd rather have the header in front of each data section to
 * avoid having two separate spaces that can run out, but then I'd either
 * lose page alignment on the data blocks or spend an entire page on the
 * header */

struct log_extent {
  uint64_t sector;
  uint32_t count;
  uint32_t offset; /* offset from start of data area to start of extent */
};

/* struct above should be 16 bytes, or 256 extents/page */

typedef struct log_extent log_request_t;
typedef struct log_extent log_response_t;

DEFINE_RING_TYPES(log, log_request_t, log_response_t);

#define LOG_HEADER_PAGES 4

#endif
