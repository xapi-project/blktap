/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#ifndef __VHD_H__
#define __VHD_H__

#include <asm/types.h>
#include <uuid/uuid.h>
#include <inttypes.h>
#include "disktypes.h"

typedef __u32 u32;
typedef __u64 u64;

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
  char   cookie[8];       /* Identifies original creator of the disk      */
  u32    features;        /* Feature Support -- see below                 */
  u32    ff_version;      /* (major,minor) version of disk file           */
  u64    data_offset;     /* Abs. offset from SOF to next structure       */
  u32    timestamp;       /* Creation time.  secs since 1/1/2000GMT       */
  char   crtr_app[4];     /* Creator application                          */
  u32    crtr_ver;        /* Creator version (major,minor)                */
  u32    crtr_os;         /* Creator host OS                              */
  u64    orig_size;       /* Size at creation (bytes)                     */
  u64    curr_size;       /* Current size of disk (bytes)                 */
  u32    geometry;        /* Disk geometry                                */
  u32    type;            /* Disk type                                    */
  u32    checksum;        /* 1's comp sum of this struct.                 */
  uuid_t uuid;            /* Unique disk ID, used for naming parents      */
  char   saved;           /* one-bit -- is this disk/VM in a saved state? */
  char   hidden;          /* tapdisk-specific field: is this vdi hidden?  */
  char   reserved[426];   /* padding                                      */
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

#define VHD_CREATOR_VERSION 0x00010001

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

/* String table for hd.type */
static const char *HD_TYPE_STR[7] = {
        "None",                    /* 0 */
        "Reserved (deprecated)",   /* 1 */
        "Fixed hard disk",         /* 2 */
        "Dynamic hard disk",       /* 3 */
        "Differencing hard disk",  /* 4 */
        "Reserved (deprecated)",   /* 5 */
        "Reserved (deprecated)"    /* 6 */
};

#define HD_TYPE_MAX 6

struct prt_loc {
  u32    code;            /* Platform code -- see defines below.          */
  u32    data_space;      /* Number of 512-byte sectors to store locator  */
  u32    data_len;        /* Actual length of parent locator in bytes     */
  u32    res;             /* Must be zero                                 */
  u64    data_offset;     /* Absolute offset of locator data (bytes)      */
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
  char   cookie[8];       /* Should contain "cxsparse"                    */
  u64    data_offset;     /* Byte offset of next record. (Unused) 0xffs   */
  u64    table_offset;    /* Absolute offset to the BAT.                  */
  u32    hdr_ver;         /* Version of the dd_hdr (major,minor)          */
  u32    max_bat_size;    /* Maximum number of entries in the BAT         */
  u32    block_size;      /* Block size in bytes. Must be power of 2.     */
  u32    checksum;        /* Header checksum.  1's comp of all fields.    */
  uuid_t prt_uuid;        /* ID of the parent disk.                       */
  u32    prt_ts;          /* Modification time of the parent disk         */
  u32    res1;            /* Reserved.                                    */
  char   prt_name[512];   /* Parent unicode name.                         */
  struct prt_loc loc[8];  /* Parent locator entries.                      */
  char   res2[256];       /* Reserved.                                    */
};

/* VHD cookie string. */
static const char DD_COOKIE[9]  =  "cxsparse";

/* Version field in hd_ftr */
#define DD_VERSION 0x00010000

/* Default blocksize is 2 meg. */
#define DD_BLOCKSIZE_DEFAULT 0x00200000

#define DD_BLK_UNUSED 0xFFFFFFFF

/* Layout of a dynamic disk:
 *
 * +-------------------------------------------------+
 * | Mirror image of HD footer (hd_ftr) (512 bytes)  |
 * +-------------------------------------------------+
 * | Sparse drive header (dd_hdr) (1024 bytes)       |
 * +-------------------------------------------------+
 * | BAT (Block allocation table)                    |
 * |   - Array of absolute sector offsets into the   |
 * |     file (u32).                                 |
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

struct disk_driver;

struct vhd_info {
        int       spb;
        int       bat_entries;
        uint32_t *bat;
        uint64_t  secs;
	long      td_fields[TD_FIELD_INVALID];
};
uint32_t vhd_footer_checksum(struct hd_ftr *footer);
uint32_t vhd_header_checksum(struct dd_hdr *header);
void vhd_get_footer(struct disk_driver *dd, struct hd_ftr *footer);
int vhd_get_header(struct disk_driver *dd, struct dd_hdr *header);
int vhd_get_info(struct disk_driver *dd, struct vhd_info *info);
int vhd_get_bat(struct disk_driver *dd, struct vhd_info *info);
int vhd_set_field(struct disk_driver *dd, td_field_t field, long value);
int vhd_coalesce(char *name);
int vhd_fill(char *name);
int vhd_repair(struct disk_driver *dd);
int vhd_read(struct disk_driver *dd, int argc, char *argv[]);

#endif
