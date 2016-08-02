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

#ifndef __VHD_H__
#define __VHD_H__

#include <inttypes.h>
#include <uuid/uuid.h>

#define DEBUG 1

/* ---------------------------------------------------------------------- */
/* General definitions.                                                   */
/* ---------------------------------------------------------------------- */

#define VHD_SECTOR_SIZE  512
#define VHD_SECTOR_SHIFT   9

/* ---------------------------------------------------------------------- */
/* This is the generic disk footer, used by all disks.                    */
/* ---------------------------------------------------------------------- */

struct hd_ftr {
  char        cookie[8];       /* Identifies original creator of the disk      */
  uint32_t    features;        /* Feature Support -- see below                 */
  uint32_t    ff_version;      /* (major,minor) version of disk file           */
  uint64_t    data_offset;     /* Abs. offset from SOF to next structure       */
  uint32_t    timestamp;       /* Creation time.  secs since 1/1/2000GMT       */
  char        crtr_app[4];     /* Creator application                          */
  uint32_t    crtr_ver;        /* Creator version (major,minor)                */
  uint32_t    crtr_os;         /* Creator host OS                              */
  uint64_t    orig_size;       /* Size at creation (bytes)                     */
  uint64_t    curr_size;       /* Current size of disk (bytes)                 */
  uint32_t    geometry;        /* Disk geometry                                */
  uint32_t    type;            /* Disk type                                    */
  uint32_t    checksum;        /* 1's comp sum of this struct.                 */
  uuid_t      uuid;            /* Unique disk ID, used for naming parents      */
  char        saved;           /* one-bit -- is this disk/VM in a saved state? */
  char        hidden;          /* tapdisk-specific field: is this vdi hidden?  */
  char        reserved[426];   /* padding                                      */
};

/* VHD cookie string. */
static const char HD_COOKIE[9]  =  "conectix";

/* Feature fields in hd_ftr */
#define HD_NO_FEATURES     0x00000000
#define HD_TEMPORARY       0x00000001 /* disk can be deleted on shutdown */
#define HD_RESERVED        0x00000002 /* NOTE: must always be set        */

/* Version field in hd_ftr */
#define HD_FF_VERSION      0x00010000

/* Known creator OS type fields in hd_ftr.crtr_os */
#define HD_CR_OS_WINDOWS   0x5769326B /* (Wi2k) */
#define HD_CR_OS_MACINTOSH 0x4D616320 /* (Mac ) */

/*
 * version 0.1:  little endian bitmaps
 * version 1.1:  big endian bitmaps; batmap
 * version 1.2:  libvhd
 * version 1.3:  batmap version bump to 1.2
 */
#define VHD_VERSION(major, minor)  (((major) << 16) | ((minor) & 0x0000FFFF))
#define VHD_CURRENT_VERSION        VHD_VERSION(1, 3)

/* Disk geometry accessor macros. */
/* Geometry is a triple of (cylinders (2 bytes), tracks (1 byte), and 
 * secotrs-per-track (1 byte)) 
 */
#define GEOM_GET_CYLS(_g)  (((_g) >> 16) & 0xffff)
#define GEOM_GET_HEADS(_g) (((_g) >> 8)  & 0xff)
#define GEOM_GET_SPT(_g)   ((_g) & 0xff)

#define GEOM_ENCODE(_c, _h, _s) (((_c) << 16) | ((_h) << 8) | (_s))

/* type field in hd_ftr */
#define HD_TYPE_NONE       0
#define HD_TYPE_FIXED      2  /* fixed-allocation disk */
#define HD_TYPE_DYNAMIC    3  /* dynamic disk */
#define HD_TYPE_DIFF       4  /* differencing disk */
#define HD_TYPE_MAX        6

struct prt_loc {
  uint32_t    code;            /* Platform code -- see defines below.          */
  uint32_t    data_space;      /* Number of 512-byte sectors to store locator  */
  uint32_t    data_len;        /* Actual length of parent locator in bytes     */
  uint32_t    res;             /* Must be zero                                 */
  uint64_t    data_offset;     /* Absolute offset of locator data (bytes)      */
};

/* Platform Codes */
#define PLAT_CODE_NONE  0x0
#define PLAT_CODE_WI2R  0x57693272  /* deprecated                         */
#define PLAT_CODE_WI2K  0x5769326B  /* deprecated                         */
#define PLAT_CODE_W2RU  0x57327275  /* Windows relative path (UTF-16)     */
#define PLAT_CODE_W2KU  0x57326B75  /* Windows absolute path (UTF-16)     */
#define PLAT_CODE_MAC   0x4D616320  /* MacOS alias stored as a blob.      */
#define PLAT_CODE_MACX  0x4D616358  /* File URL (UTF-8), see RFC 2396.    */

/* ---------------------------------------------------------------------- */
/* This is the dynamic disk header.                                       */
/* ---------------------------------------------------------------------- */

struct dd_hdr {
  char        cookie[8];       /* Should contain "cxsparse"                    */
  uint64_t    data_offset;     /* Byte offset of next record. (Unused) 0xffs   */
  uint64_t    table_offset;    /* Absolute offset to the BAT.                  */
  uint32_t    hdr_ver;         /* Version of the dd_hdr (major,minor)          */
  uint32_t    max_bat_size;    /* Maximum number of entries in the BAT         */
  uint32_t    block_size;      /* Block size in bytes. Must be power of 2.     */
  uint32_t    checksum;        /* Header checksum.  1's comp of all fields.    */
  uuid_t      prt_uuid;        /* ID of the parent disk.                       */
  uint32_t    prt_ts;          /* Modification time of the parent disk         */
  uint32_t    res1;            /* Reserved.                                    */
  char        prt_name[512];   /* Parent unicode name.                         */
  struct prt_loc loc[8];  /* Parent locator entries.                      */
  char        res2[256];       /* Reserved.                                    */
};

/* VHD cookie string. */
static const char DD_COOKIE[9]  =  "cxsparse";

/* Version field in hd_ftr */
#define DD_VERSION 0x00010000

/* Default blocksize is 2 meg. */
#define DD_BLOCKSIZE_DEFAULT 0x00200000

#define DD_BLK_UNUSED 0xFFFFFFFF

struct dd_batmap_hdr {
  char        cookie[8];       /* should contain "tdbatmap"                    */
  uint64_t    batmap_offset;   /* byte offset to batmap                        */
  uint32_t    batmap_size;     /* batmap size in sectors                       */
  uint32_t    batmap_version;  /* version of batmap                            */
  uint32_t    checksum;        /* batmap checksum -- 1's complement of batmap  */
  char        marker;          /* generic marker field                         */
};

static const char VHD_BATMAP_COOKIE[9] = "tdbatmap";

/*
 * version 1.1: signed char checksum
 */
#define VHD_BATMAP_VERSION(major, minor)  (((major) << 16) | ((minor) & 0x0000FFFF))
#define VHD_BATMAP_CURRENT_VERSION        VHD_BATMAP_VERSION(1, 2)

/* Layout of a dynamic disk:
 *
 * +-------------------------------------------------+
 * | Mirror image of HD footer (hd_ftr) (512 bytes)  |
 * +-------------------------------------------------+
 * | Sparse drive header (dd_hdr) (1024 bytes)       |
 * +-------------------------------------------------+
 * | BAT (Block allocation table)                    |
 * |   - Array of absolute sector offsets into the   |
 * |     file (uint32_t).                            |
 * |   - Rounded up to a sector boundary.            |
 * |   - Unused entries are marked as 0xFFFFFFFF     |
 * |   - max entries in dd_hdr->max_bat_size         |
 * +-------------------------------------------------+
 * | Data Block 0                                    |
 * | Bitmap (padded to 512 byte sector boundary)     |
 * |   - each bit indicates whether the associated   |
 * |     sector within this block is used.           |
 * | Data                                            |
 * |   - power-of-two multiple of sectors.           |
 * |   - default 2MB (4096 * 512)                    |
 * |   - Any entries with zero in bitmap should be   |
 * |     zero on disk                                |
 * +-------------------------------------------------+
 * | Data Block 1                                    |
 * +-------------------------------------------------+
 * | ...                                             |
 * +-------------------------------------------------+
 * | Data Block n                                    |
 * +-------------------------------------------------+
 * | HD Footer (511 bytes)                           |
 * +-------------------------------------------------+
 */

#endif
