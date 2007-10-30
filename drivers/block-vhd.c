/* block-vhd.c
 *
 * asynchronous vhd implementation.
 *
 * Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 *
 * A note on write transactions:
 * Writes that require updating the BAT or bitmaps cannot be signaled
 * as complete until all updates have reached disk.  Transactions are
 * used to ensure proper ordering in these cases.  The two types of
 * transactions are as follows:
 *   - Bitmap updates only: data writes that require updates to the same
 *     bitmap are grouped in a transaction.  Only after all data writes
 *     in a transaction complete does the bitmap write commence.  Only
 *     after the bitmap write finishes are the data writes signalled as
 *     complete.
 *   - BAT and bitmap updates: data writes are grouped in transactions
 *     as above, but a special extra write is included in the transaction,
 *     which zeros out the newly allocated bitmap on disk.  When the data
 *     writes and the zero-bitmap write complete, the BAT and bitmap writes
 *     are started in parallel.  The transaction is completed only after both
 *     the BAT and bitmap writes successfully return.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <uuid/uuid.h> /* For whatever reason, Linux packages this in */
                       /* e2fsprogs-devel.                            */
#include <string.h>    /* for memset.                                 */
#include <libaio.h>
#include <iconv.h>
#include <libgen.h>
#include <endian.h>
#include <byteswap.h>
#include <inttypes.h>

#include "tapdisk.h"
#include "vhd.h"
#include "profile.h"
#include "atomicio.h"
#include "io-optimize.h"

unsigned int SPB;

#define DEBUGGING   2
#define ASSERTING   1
#define PREALLOCATE_BLOCKS 1

#define __TRACE(s)                                                             \
do {                                                                           \
	DBG("%s: QUEUED: %" PRIu64 ", COMPLETED: %" PRIu64 ", "                \
	    "RETURNED: %" PRIu64 ", DATA_ALLOCATED: %lu, BBLK: %u\n",          \
	    s->name, s->queued, s->completed, s->returned,                     \
	    VHD_REQS_DATA - s->vreq_free_count, s->bat.pbw_blk);               \
} while(0)

#define __ASSERT(_p)                                                           \
if ( !(_p) ) {                                                                 \
	DPRINTF("%s:%d: FAILED ASSERTION: '%s'\n", __FILE__, __LINE__, #_p);   \
	DBG("%s:%d: FAILED ASSERTION: '%s'\n", __FILE__, __LINE__, #_p);       \
	tlog_flush("tapdisk-assert", log);                                     \
	*(int*)0 = 0;                                                          \
}

static struct tlog *log;

#if (DEBUGGING == 1)
  #define DBG(_f, _a...)             DPRINTF(_f, ##_a)
  #define TRACE(s)                   ((void)0)
#elif (DEBUGGING == 2)
  #define DBG(_f, _a...)             tlog_write(log, _f, ##_a)
  #define TRACE(s)                   __TRACE(s)
#else
  #define DBG(_f, _a...)             ((void)0)
  #define TRACE(s)                   ((void)0)
#endif

#if (ASSERTING == 1)
  #define ASSERT(_p)                 __ASSERT(_p)
#else
  #define ASSERT(_p)                 ((void)0)
#endif

/******VHD DEFINES******/
#define VHD_CACHE_SIZE               32

#define VHD_REQS_DATA                TAPDISK_DATA_REQUESTS
#define VHD_REQS_META                (VHD_CACHE_SIZE + 2)
#define VHD_REQS_TOTAL               (VHD_REQS_DATA + VHD_REQS_META)

#define VHD_OP_BAT_WRITE             0
#define VHD_OP_DATA_READ             1
#define VHD_OP_DATA_WRITE            2
#define VHD_OP_BITMAP_READ           3
#define VHD_OP_BITMAP_WRITE          4
#define VHD_OP_ZERO_BM_WRITE         5

#define VHD_BM_BAT_LOCKED            0
#define VHD_BM_BAT_CLEAR             1
#define VHD_BM_BIT_CLEAR             2
#define VHD_BM_BIT_SET               3
#define VHD_BM_NOT_CACHED            4
#define VHD_BM_READ_PENDING          5

#define VHD_FLAG_OPEN_RDONLY         1
#define VHD_FLAG_OPEN_NO_CACHE       2
#define VHD_FLAG_OPEN_QUIET          4
#define VHD_FLAG_OPEN_STRICT         8
#define VHD_FLAG_OPEN_QUERY          16

#define VHD_FLAG_BAT_LOCKED          1
#define VHD_FLAG_BAT_WRITE_STARTED   2

#define VHD_FLAG_BM_UPDATE_BAT       1
#define VHD_FLAG_BM_WRITE_PENDING    2
#define VHD_FLAG_BM_READ_PENDING     4
#define VHD_FLAG_BM_LOCKED           8

#define VHD_FLAG_REQ_UPDATE_BAT      1
#define VHD_FLAG_REQ_UPDATE_BITMAP   2
#define VHD_FLAG_REQ_QUEUED          4
#define VHD_FLAG_REQ_FINISHED        8

#define VHD_FLAG_TX_LIVE             1
#define VHD_FLAG_TX_UPDATE_BAT       2

#define VHD_FLAG_CR_SPARSE           1
#define VHD_FLAG_CR_IGNORE_PARENT    2

typedef uint8_t vhd_flag_t;

struct vhd_request;

struct vhd_req_list {
	struct vhd_request *head, *tail;
};

struct vhd_transaction {
	int error;
	int closed;
	int started;
	int finished;
	vhd_flag_t status;
	struct vhd_req_list requests;
};

struct vhd_request {
	int id;
	int error;
	char *buf;
	uint8_t op;
	int nr_secs;
	uint64_t lsec;                         /* logical disk sector */
	void *private;
	vhd_flag_t flags;
	td_callback_t cb;
	struct tiocb tiocb;
	struct vhd_request *next;
	struct vhd_transaction *tx;
};

struct vhd_bat {
	uint32_t  *bat;
	vhd_flag_t status;
	uint32_t   pbw_blk;                    /* blk num of pending write */
	uint64_t   pbw_offset;                 /* file offset of same */
	struct vhd_request req;                /* for writing bat table */
	struct vhd_request zero_req;           /* for initializing bitmaps */
};

struct vhd_bitmap {
	u32        blk;
	u64        seqno;                      /* lru sequence number */
	vhd_flag_t status;

	char *map;                             /* map should only be modified
					        * in finish_bitmap_write */
	char *shadow;                          /* in-memory bitmap changes are 
					        * made to shadow and copied to
					        * map only after having been
					        * flushed to disk */
	struct vhd_transaction tx;             /* transaction data structure
						* encapsulating data, bitmap, 
						* and bat writes */
	struct vhd_req_list queue;             /* data writes waiting for next
						* transaction */
	struct vhd_req_list waiting;           /* pending requests that cannot
					        * be serviced until this bitmap
					        * is read from disk */
	struct vhd_request req;
};

struct vhd_state {
	int                   fd;
	vhd_flag_t            flags;
	int                   bitmap_format;

        /* VHD stuff */
        struct hd_ftr         ftr;
        struct dd_hdr         hdr;
	u32                   spp;             /* sectors per page */
        u32                   spb;             /* sectors per block */
        u64                   next_db;         /* pointer to the next 
						* (unallocated) datablock */

	struct vhd_bat        bat;

	u64                   bm_lru;          /* lru sequence number */
	u32                   bm_secs;         /* size of bitmap, in sectors */
	struct vhd_bitmap    *bitmap[VHD_CACHE_SIZE];

	int                   bm_free_count;
	struct vhd_bitmap    *bitmap_free[VHD_CACHE_SIZE];
	struct vhd_bitmap     bitmap_list[VHD_CACHE_SIZE];

	int                   vreq_free_count;
	struct vhd_request   *vreq_free[VHD_REQS_DATA];
	struct vhd_request    vreq_list[VHD_REQS_DATA];

	char                 *name;

	struct disk_driver   *dd;

#if (PREALLOCATE_BLOCKS == 1)
	char                 *zeros;
	int                   zsize;
#endif

	/* debug info */
	struct profile_info   tp;
	uint64_t queued, completed, returned;
	uint64_t writes, reads, write_size, read_size;
};

/* Helpers: */
#if BYTE_ORDER == LITTLE_ENDIAN
  #define BE32_IN(foo)  (*(foo)) = bswap_32(*(foo))
  #define BE64_IN(foo)  (*(foo)) = bswap_64(*(foo))
  #define BE32_OUT(foo) (*(foo)) = bswap_32(*(foo))
  #define BE64_OUT(foo) (*(foo)) = bswap_64(*(foo))
#else
  #define BE32_IN(foo)
  #define BE64_IN(foo)
  #define BE32_OUT(foo)
  #define BE64_OUT(foo)
#endif

#define MIN(a, b)                  (((a) < (b)) ? (a) : (b))

#define test_vhd_flag(word, flag)  ((word) & (flag))
#define set_vhd_flag(word, flag)   ((word) |= (flag))
#define clear_vhd_flag(word, flag) ((word) &= ~(flag))

#define bat_entry(s, blk)          ((s)->bat.bat[(blk)])

#define secs_round_up(bytes) \
              (((bytes) + (VHD_SECTOR_SIZE - 1)) >> VHD_SECTOR_SHIFT)

static int finish_data_transaction(struct disk_driver *, struct vhd_bitmap *);

static inline int
le_test_bit (int nr, volatile u32 *addr)
{
	return (((u32 *)addr)[nr >> 5] >> (nr & 31)) & 1;
}

static inline void
le_clear_bit (int nr, volatile u32 *addr)
{
	((u32 *)addr)[nr >> 5] &= ~(1 << (nr & 31));
}

static inline void
le_set_bit (int nr, volatile u32 *addr)
{
	((u32 *)addr)[nr >> 5] |= (1 << (nr & 31));
}

#define BIT_MASK 0x80

static inline int
be_test_bit (int nr, volatile char *addr)
{
	return ((addr[nr >> 3] << (nr & 7)) & BIT_MASK) != 0;
}

static inline void
be_clear_bit (int nr, volatile char *addr)
{
	addr[nr >> 3] &= ~(BIT_MASK >> (nr & 7));
}

static inline void
be_set_bit (int nr, volatile char *addr)
{
	addr[nr >> 3] |= (BIT_MASK >> (nr & 7));
}

#define test_bit(s, nr, addr)                                             \
	((s)->bitmap_format == LITTLE_ENDIAN ?                            \
	 le_test_bit(nr, (uint32_t *)(addr)) : be_test_bit(nr, addr))

#define clear_bit(s, nr, addr)                                            \
	((s)->bitmap_format == LITTLE_ENDIAN ?                            \
	 le_clear_bit(nr, (uint32_t *)(addr)) : be_clear_bit(nr, addr))

#define set_bit(s, nr, addr)                                              \
	((s)->bitmap_format == LITTLE_ENDIAN ?                            \
	 le_set_bit(nr, (uint32_t *)(addr)) : be_set_bit(nr, addr))

/* Debug print functions: */

/* Stringify the VHD timestamp for printing.                              */
/* As with ctime_r, target must be >=26 bytes.                            */
/* TODO: Verify this is correct.                                          */
static size_t 
vhd_time_to_s(u32 timestamp, char *target)
{
        struct tm tm;
        time_t t1, t2;
        char *cr;
    
        memset(&tm, 0, sizeof(struct tm));
 
        /* VHD uses an epoch of 12:00AM, Jan 1, 2000.         */
        /* Need to adjust this to the expected epoch of 1970. */
        tm.tm_year  = 100;
        tm.tm_mon   = 0;
        tm.tm_mday  = 1;

        t1 = mktime(&tm);
        t2 = t1 + (time_t)timestamp;
        ctime_r(&t2, target);

        /* handle mad ctime_r newline appending. */
        if ((cr = strchr(target, '\n')) != NULL)
		*cr = '\0';

        return (strlen(target));
}

static u32
vhd_time(time_t time)
{
	struct tm tm;
	time_t micro_epoch;

	memset(&tm, 0, sizeof(struct tm));
	tm.tm_year   = 100;
	tm.tm_mon    = 0;
	tm.tm_mday   = 1;
	micro_epoch  = mktime(&tm);

	return (u32)(time - micro_epoch);
}

/*
 * nabbed from vhd specs.
 */
static u32
chs(uint64_t size)
{
	u32 secs, cylinders, heads, spt, cth;

	secs = secs_round_up(size);

	if (secs > 65535 * 16 * 255)
		secs = 65535 * 16 * 255;

	if (secs >= 65535 * 16 * 63) {
		spt   = 255;
		cth   = secs / spt;
		heads = 16;
	} else {
		spt   = 17;
		cth   = secs / spt;
		heads = (cth + 1023) / 1024;

		if (heads < 4)
			heads = 4;

		if (cth >= (heads * 1024) || heads > 16) {
			spt   = 31;
			cth   = secs / spt;
			heads = 16;
		}

		if (cth >= heads * 1024) {
			spt   = 63;
			cth   = secs / spt;
			heads = 16;
		}
	}

	cylinders = cth / heads;

	return GEOM_ENCODE(cylinders, heads, spt);
}

uint32_t
vhd_footer_checksum( struct hd_ftr *f )
{
	int i;
	u32 cksm = 0;
	struct hd_ftr safe_f; /* 512 bytes on the stack should be okay here. */
	unsigned char *blob;
	memcpy(&safe_f, f, sizeof(struct hd_ftr));

	safe_f.checksum = 0;
    
	blob = (unsigned char *) &safe_f;
	for (i = 0; i < sizeof(struct hd_ftr); i++)
		cksm += (u32)blob[i];
    
	return ~cksm;
}

static void
debug_print_footer( struct hd_ftr *f )
{
	u32  ff_maj, ff_min;
	char time_str[26];
	char creator[5];
	u32  cr_maj, cr_min;
	u64  c, h, s;
	u32  cksm, cksm_save;
	char uuid[37];

	DPRINTF("VHD Footer Summary:\n-------------------\n");
	DPRINTF("Features            : (0x%08x) %s%s\n", f->features,
		(f->features & HD_TEMPORARY) ? "<TEMP>" : "",
		(f->features & HD_RESERVED)  ? "<RESV>" : "");

	ff_maj = f->ff_version >> 16;
	ff_min = f->ff_version & 0xffff;
	DPRINTF("File format version : Major: %d, Minor: %d\n", 
		ff_maj, ff_min);

	DPRINTF("Data offset         : %lld\n", f->data_offset);
    
	vhd_time_to_s(f->timestamp, time_str);
	DPRINTF("Timestamp           : %s\n", time_str);
    
	memcpy(creator, f->crtr_app, 4);
	creator[4] = '\0';
	DPRINTF("Creator Application : '%s'\n", creator);

	cr_maj = f->crtr_ver >> 16;
	cr_min = f->crtr_ver & 0xffff;
	DPRINTF("Creator version     : Major: %d, Minor: %d\n",
		cr_maj, cr_min);

	DPRINTF("Creator OS          : %s\n",
		((f->crtr_os == HD_CR_OS_WINDOWS) ? "Windows" :
		 ((f->crtr_os == HD_CR_OS_MACINTOSH) ? "Macintosh" : 
		  "Unknown!")));

	DPRINTF("Original disk size  : %lld MB (%lld Bytes)\n",
		f->orig_size >> 20, f->orig_size);
	
	DPRINTF("Current disk size   : %lld MB (%lld Bytes)\n",
		f->curr_size >> 20, f->curr_size);

	c = f->geometry >> 16;
	h = (f->geometry & 0x0000FF00) >> 8;
	s = f->geometry & 0x000000FF;
	DPRINTF("Geometry            : Cyl: %lld, Hds: %lld, Sctrs: %lld\n",
		c, h, s);
	DPRINTF("                    : = %lld MB (%lld Bytes)\n", 
		(c*h*s)>>11, c*h*s<<9);
	
	DPRINTF("Disk type           : %s\n", 
		f->type <= HD_TYPE_MAX ? 
		HD_TYPE_STR[f->type] : "Unknown type!\n");

	cksm = vhd_footer_checksum(f);
	DPRINTF("Checksum            : 0x%x|0x%x (%s)\n", f->checksum, cksm,
		f->checksum == cksm ? "Good!" : "Bad!" );

	uuid_unparse(f->uuid, uuid);
	DPRINTF("UUID                : %s\n", uuid);
	
	DPRINTF("Saved state         : %s\n", f->saved == 0 ? "No" : "Yes" );
}

uint32_t
vhd_header_checksum( struct dd_hdr *h )
{
	int i;
	u32 cksm = 0;
	struct dd_hdr safe_h; /* slightly larger for this one. */
	unsigned char *blob;
	memcpy(&safe_h, h, sizeof(struct dd_hdr));

	safe_h.checksum = 0;
    
	blob = (unsigned char *) &safe_h;
	for (i = 0; i < sizeof(struct dd_hdr); i++)
		cksm += (u32)blob[i];
    
	return ~cksm;
}

static void
debug_print_header( struct dd_hdr *h )
{
	char uuid[37];
	char time_str[26];
	u32  cksm;

	DPRINTF("VHD Header Summary:\n-------------------\n");
	DPRINTF("Data offset (unusd) : %lld\n", h->data_offset);
	DPRINTF("Table offset        : %lld\n", h->table_offset);
	DPRINTF("Header version      : 0x%08x\n", h->hdr_ver);
	DPRINTF("Max BAT size        : %d\n", h->max_bat_size);
	DPRINTF("Block size          : 0x%x (%dMB)\n", h->block_size,
		h->block_size >> 20);

	uuid_unparse(h->prt_uuid, uuid);
	DPRINTF("Parent UUID         : %s\n", uuid);
    
	vhd_time_to_s(h->prt_ts, time_str);
	DPRINTF("Parent timestamp    : %s\n", time_str);

	cksm = vhd_header_checksum(h);
	DPRINTF("Checksum            : 0x%x|0x%x (%s)\n", h->checksum, cksm,
		h->checksum == cksm ? "Good!" : "Bad!" );

	{
		int i;
		for (i = 0; i < 8; i++)
			DPRINTF("loc[%d].offset: %llu\n",
				i, h->loc[i].data_offset);
	}
}

/* End of debug print functions. */

static int
vhd_read_hd_ftr(int fd, struct hd_ftr *ftr, vhd_flag_t flags)
{
	char *buf;
	int err, secs;
	off_t vhd_end;

	err  = -EINVAL;
	secs = secs_round_up(sizeof(struct hd_ftr));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return -ENOMEM;

	memset(ftr, 0, sizeof(struct hd_ftr));

	/* Not sure if it's generally a good idea to use SEEK_END with -ve   */
	/* offsets, so do one seek to find the end of the file, and then use */
	/* SEEK_SETs when searching for the footer.                          */
	if ((vhd_end = lseek64(fd, 0, SEEK_END)) == -1) {
		err = -errno;
		goto out;
	}

	/* Look for the footer 512 bytes before the end of the file. */
	if (lseek64(fd, (off64_t)(vhd_end - 512), SEEK_SET) == -1) {
		err = -errno;
		goto out;
	}
	if (read(fd, buf, 512) != 512) {
		err = (errno ? -errno : -EIO);
		goto out;
	}
	memcpy(ftr, buf, sizeof(struct hd_ftr));
	if (memcmp(ftr->cookie, HD_COOKIE,  8) == 0) goto found_footer;
    
	/* According to the spec, pre-Virtual PC 2004 VHDs used a            */
	/* 511B footer.  Try that...                                         */
	memcpy(ftr, &buf[1], MIN(sizeof(struct hd_ftr), 511));
	if (memcmp(ftr->cookie, HD_COOKIE,  8) == 0) goto found_footer;
    
	/* Last try.  Look for the copy of the footer at the start of image. */
	if (test_vhd_flag(flags, VHD_FLAG_OPEN_STRICT)) {
		DPRINTF("NOTE: Couldn't find footer at the end of the VHD "
			"image.  Using backup footer from start of file.  "
			"This may have been caused by system crash recovery, "
			"or this VHD may be corrupt.\n");
		{
			int *i = (int *)buf;
			if (i[0] == 0xc7c7c7c7)
				DPRINTF("footer dead\n");
		}
	}
	if (lseek64(fd, 0, SEEK_SET) == -1) {
		err = -errno;
		goto out;
	}
	if (read(fd, buf, 512) != 512) {
		err = (errno ? -errno : -EIO);
		goto out;
	}
	memcpy(ftr, buf, sizeof(struct hd_ftr));
	if (memcmp(ftr->cookie, HD_COOKIE,  8) == 0) goto found_footer;

	if (!test_vhd_flag(flags, VHD_FLAG_OPEN_QUIET))
		DPRINTF("error reading footer.\n");
	goto out;

 found_footer:

	err = 0;

	BE32_IN(&ftr->features);
	BE32_IN(&ftr->ff_version);
	BE64_IN(&ftr->data_offset);
	BE32_IN(&ftr->timestamp);
	BE32_IN(&ftr->crtr_ver);
	BE32_IN(&ftr->crtr_os);
	BE64_IN(&ftr->orig_size);
	BE64_IN(&ftr->curr_size);
	BE32_IN(&ftr->geometry);
	BE32_IN(&ftr->type);
	BE32_IN(&ftr->checksum);

 out:
	free(buf);
	return err;
}

static int
vhd_kill_hd_ftr(int fd)
{
	int err = 1;
	off64_t end;
	char *zeros;

	if (posix_memalign((void **)&zeros, 512, 512) == -1)
		return -errno;
	memset(zeros, 0xc7c7c7c7, 512);

	if ((end = lseek64(fd, 0, SEEK_END)) == -1)
		goto fail;

	if (lseek64(fd, (end - 512), SEEK_SET) == -1)
		goto fail;

	if (write(fd, zeros, 512) != 512)
		goto fail;

	err = 0;

 fail:
	free(zeros);
	if (err)
		return (errno ? -errno : -EIO);
	return 0;
}

/* 
 * Take a copy of the footer on the stack and update endianness.
 * Write it to the current position in the fd.
 */
static int
vhd_write_hd_ftr(int fd, struct hd_ftr *in_use_ftr)
{
	char *buf;
	int ret, secs, err;
	struct hd_ftr ftr = *in_use_ftr;

	err  = 0;
	secs = secs_round_up(sizeof(struct hd_ftr));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return -ENOMEM;

	BE32_OUT(&ftr.features);
	BE32_OUT(&ftr.ff_version);
	BE64_OUT(&ftr.data_offset);
	BE32_OUT(&ftr.timestamp);
	BE32_OUT(&ftr.crtr_ver);
	BE32_OUT(&ftr.crtr_os);
	BE64_OUT(&ftr.orig_size);
	BE64_OUT(&ftr.curr_size);
	BE32_OUT(&ftr.geometry);
	BE32_OUT(&ftr.type);
	BE32_OUT(&ftr.checksum);

	memcpy(buf, &ftr, sizeof(struct hd_ftr));
	
	ret = write(fd, buf, 512);
	if (ret != 512)
		err = (errno ? -errno : -EIO);

	free(buf);
	return err;
}

/* 
 * Take a copy of the header on the stack and update endianness.
 * Write it to the current position in the fd. 
 */
static int
vhd_write_dd_hdr(int fd, struct dd_hdr *in_use_hdr)
{
	char *buf;
	int ret, secs, err, i, n;
	struct dd_hdr hdr = *in_use_hdr;

	err  = 0;
	secs = secs_round_up(sizeof(struct dd_hdr));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return -ENOMEM;

	BE64_OUT(&hdr.data_offset);
	BE64_OUT(&hdr.table_offset);
	BE32_OUT(&hdr.hdr_ver);
	BE32_OUT(&hdr.max_bat_size);	
	BE32_OUT(&hdr.block_size);
	BE32_OUT(&hdr.checksum);
	BE32_OUT(&hdr.prt_ts);

	n = sizeof(hdr.loc) / sizeof(struct prt_loc);
	for (i = 0; i < n; i++) {
		BE32_OUT(&hdr.loc[i].code);
		BE32_OUT(&hdr.loc[i].data_space);
		BE32_OUT(&hdr.loc[i].data_len);
		BE64_OUT(&hdr.loc[i].data_offset);
	}

	memcpy(buf, &hdr, sizeof(struct dd_hdr));

	ret = write(fd, buf, 1024);
	if (ret != 1024)
		err = (errno ? -errno : -EIO);

	free(buf);
	return err;
}

static int
vhd_read_dd_hdr(int fd, struct dd_hdr *hdr, u64 location)
{
	char *buf;
	int   err, size, i;

	err  = -EINVAL;
	size = secs_round_up(sizeof(struct dd_hdr)) << VHD_SECTOR_SHIFT;

	if (posix_memalign((void **)&buf, 512, size))
		return -ENOMEM;

	if (lseek64(fd, location, SEEK_SET) == -1) {
		err = -errno;
		goto out;
	}
	if (read(fd, buf, size) != size) {
		err = (errno ? -errno : -EIO);
		goto out;
	}
	memcpy(hdr, buf, sizeof(struct dd_hdr));
	if (memcmp(hdr->cookie, DD_COOKIE,  8) != 0)
		goto out;
	
	err = 0;

	BE64_IN(&hdr->data_offset);
	BE64_IN(&hdr->table_offset);
	BE32_IN(&hdr->hdr_ver);
	BE32_IN(&hdr->max_bat_size);
	BE32_IN(&hdr->block_size);
	BE32_IN(&hdr->checksum);
	BE32_IN(&hdr->prt_ts);

	for (i = 0; i < 8; i++) {
		BE32_IN(&hdr->loc[i].code);
		BE32_IN(&hdr->loc[i].data_space);
		BE32_IN(&hdr->loc[i].data_len);
		BE64_IN(&hdr->loc[i].data_offset);
	}
	
 out:
	free(buf);
	return err;
}

static int
vhd_read_bat(int fd, struct vhd_state *s)
{
	char *buf;
	int i, secs, count = 0, err = 0;

	u32 entries  = s->hdr.max_bat_size;
	u64 location = s->hdr.table_offset;
	secs         = secs_round_up(entries * sizeof(u32));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return -ENOMEM;

	DBG("Reading BAT at %lld, %d entries.\n", location, entries);

	if (lseek64(fd, location, SEEK_SET) == (off64_t)-1) {
		err = -errno;
		goto out;
	}
	if (read(fd, buf, secs << VHD_SECTOR_SHIFT)
	    != (secs << VHD_SECTOR_SHIFT) ) {
		err = (errno ? -errno : -EIO);
		goto out;
	}

	memcpy(s->bat.bat, buf, entries * sizeof(u32));
	s->next_db  = location >> VHD_SECTOR_SHIFT; /* BAT is sector aligned. */
	s->next_db += secs_round_up(sizeof(u32) * entries);

	DBG("FirstDB: %llu\n", s->next_db);

	for (i = 0; i < entries; i++) {
		BE32_IN(&bat_entry(s, i));
		if ((bat_entry(s, i) != DD_BLK_UNUSED) && 
		    (bat_entry(s, i) >= s->next_db)) {
			s->next_db = bat_entry(s, i) + s->spb + s->bm_secs;
			DBG("i: %d, bat[i]: %u, spb: %d, next: %llu\n",
			    i, bat_entry(s, i), s->spb,  s->next_db);
		}

		if (bat_entry(s, i) != DD_BLK_UNUSED) count++;
	}

	/* ensure that data region of segment begins on page boundary */
	if ((s->next_db + s->bm_secs) % s->spp)
		s->next_db += (s->spp - ((s->next_db + s->bm_secs) % s->spp));
    
	DBG("NextDB: %llu\n", s->next_db);
	DBG("Read BAT.  This vhd has %d full and %d unfilled data "
	    "blocks.\n", count, entries - count);

 out:
	free(buf);
	return err;
}

static void
free_bat(struct vhd_state *s)
{
	free(s->bat.bat);
	free(s->bat.req.buf);
#if (PREALLOCATE_BLOCKS == 1)
	free(s->zeros);
	s->zeros = NULL;
#else
	free(s->bat.zero_req.buf);
#endif
	memset(&s->bat, 0, sizeof(struct vhd_bat));
}

static int
alloc_bat(struct vhd_state *s)
{
	memset(&s->bat, 0, sizeof(struct vhd_bat));
	s->bat.bat = calloc(1, s->hdr.max_bat_size * sizeof(u32));
	if (!s->bat.bat)
		return -ENOMEM;

#if (PREALLOCATE_BLOCKS == 1)
	s->zsize = ((getpagesize() >> VHD_SECTOR_SHIFT) + s->spb) 
				   << VHD_SECTOR_SHIFT;
	if (posix_memalign((void **)&s->zeros, VHD_SECTOR_SIZE, s->zsize)) {
		free_bat(s);
		return -ENOMEM;
	}
	memset(s->zeros, 0x5a5a5a5a, s->zsize);
	memset(s->zeros, 0, getpagesize());
#else
	if (posix_memalign((void **)&s->bat.zero_req.buf,
			   VHD_SECTOR_SIZE, s->bm_secs << VHD_SECTOR_SHIFT)) {
		free_bat(s);
		return -ENOMEM;
	}
	memset(s->bat.zero_req.buf, 0, s->bm_secs << VHD_SECTOR_SHIFT);
#endif

	if (posix_memalign((void **)&s->bat.req.buf, 
			   VHD_SECTOR_SIZE, VHD_SECTOR_SIZE)) {
		free_bat(s);
		return -ENOMEM;
	}

	return 0;
}

static int
bitmap_format(struct vhd_state *s)
{
	int format = BIG_ENDIAN;

	if (!strncmp(s->ftr.crtr_app, "tap", 4) &&
	    s->ftr.crtr_ver == 0x00000001)
		format = LITTLE_ENDIAN;

	if (!test_vhd_flag(s->flags, VHD_FLAG_OPEN_QUIET))
		DPRINTF("%s VHD bitmap format: %s_ENDIAN\n",
			s->name, (format == BIG_ENDIAN ? "BIG" : "LITTLE"));

	return format;
}

static int
__vhd_open(struct disk_driver *dd, const char *name, vhd_flag_t flags)
{
	char *tmp;
	u32 map_size;
        int fd, ret = 0, i, o_flags;
	struct td_state  *tds = dd->td_state;
	struct vhd_state *s   = (struct vhd_state *)dd->private;

	memset(s, 0, sizeof(struct vhd_state));

        DBG("vhd_open: %s\n", name);

	o_flags  = O_LARGEFILE | O_DIRECT;
	o_flags |= ((test_vhd_flag(flags, VHD_FLAG_OPEN_RDONLY)) ? 
		    O_RDONLY : O_RDWR);
        fd = open(name, o_flags);
        if ((fd == -1) && (errno == EINVAL)) {
                /* Maybe O_DIRECT isn't supported. */
		o_flags &= ~O_DIRECT;
                fd = open(name, o_flags);
                if (fd != -1) 
			DPRINTF("WARNING: Accessing image without"
				"O_DIRECT! (%s)\n", name);
        } else if (fd != -1) 
		DBG("open(%s) with O_DIRECT\n", name);

	if (fd == -1) {
		if (!test_vhd_flag(flags, VHD_FLAG_OPEN_QUIET))
			DPRINTF("Unable to open [%s] (%d)!\n", name, -errno);
		return -errno;
	}

        /* Read the disk footer. */
        ret = vhd_read_hd_ftr(fd, &s->ftr, flags);
	if (ret) {
		if (!test_vhd_flag(flags, VHD_FLAG_OPEN_QUIET))
			DPRINTF("Error reading VHD footer.\n");
                return ret;
        }

#if (DEBUGGING == 1)
        debug_print_footer(&s->ftr);
#endif

	s->spb = s->spp = 1;

        /* If this is a dynamic or differencing disk, read the dd header. */
        if ((s->ftr.type == HD_TYPE_DYNAMIC) ||
	    (s->ftr.type == HD_TYPE_DIFF)) {

		ret = vhd_read_dd_hdr(fd, &s->hdr, s->ftr.data_offset);
                if (ret) {
			if (!test_vhd_flag(flags, VHD_FLAG_OPEN_QUIET))
				DPRINTF("Error reading VHD DD header.\n");
                        return ret;
                }

                if (s->hdr.hdr_ver != 0x00010000) {
                        DPRINTF("DANGER: unsupported hdr version! (0x%x)\n",
				 s->hdr.hdr_ver);
			if (!test_vhd_flag(flags, VHD_FLAG_OPEN_QUERY))
				return -EINVAL;
                }
#if (DEBUGGING == 1)
                debug_print_header(&s->hdr);
#endif

		s->spp     = getpagesize() >> VHD_SECTOR_SHIFT;
                s->spb     = s->hdr.block_size >> VHD_SECTOR_SHIFT;
		s->bm_secs = secs_round_up(s->spb >> 3);

		SPB = s->spb;

		if (test_vhd_flag(flags, VHD_FLAG_OPEN_NO_CACHE))
			goto out;

                /* Allocate and read the Block Allocation Table. */
		if (alloc_bat(s)) {
                        DPRINTF("Error allocating BAT.\n");
			return -ENOMEM;
                }

                if ((ret = vhd_read_bat(fd, s)) != 0) {
                        DPRINTF("Error reading BAT.\n");
			goto fail;
                }

		/* Allocate bitmap cache */
		s->bm_lru        = 0;
		map_size         = s->bm_secs << VHD_SECTOR_SHIFT;
		s->bm_free_count = VHD_CACHE_SIZE;

		ret = -ENOMEM;
		for (i = 0; i < VHD_CACHE_SIZE; i++) {
			struct vhd_bitmap *bm = &s->bitmap_list[i];
			if (posix_memalign((void **)&bm->map, 512, map_size))
				goto fail;
			if (posix_memalign((void **)&bm->shadow, 
					   512, map_size))
				goto fail;
			memset(bm->map, 0, map_size);
			memset(bm->shadow, 0, map_size);
			s->bitmap_free[i] = bm;
		}
        }

 out:
	s->vreq_free_count = VHD_REQS_DATA;
	for (i = 0; i < VHD_REQS_DATA; i++)
		s->vreq_free[i] = s->vreq_list + i;

	tmp = rindex(name, '/');
	if (tmp)
		s->name  = strdup(++tmp);

	s->dd            = dd;
	s->fd            = fd;
	s->flags         = flags;
	s->bitmap_format = bitmap_format(s);
        tds->size        = s->ftr.curr_size >> VHD_SECTOR_SHIFT;
        tds->sector_size = VHD_SECTOR_SIZE;
        tds->info        = 0;
	log              = dd->log;

        DBG("vhd_open: done (sz:%llu, sct:%lu, inf:%u)\n",
	    tds->size, tds->sector_size, tds->info);

	tp_open(&s->tp, s->name, "/tmp/vhd_log.txt", 100);

	if (test_vhd_flag(flags, VHD_FLAG_OPEN_STRICT) && 
	    !test_vhd_flag(flags, VHD_FLAG_OPEN_RDONLY)) {
		ret = vhd_kill_hd_ftr(fd);
		if (ret) {
			DPRINTF("ERROR killing footer: %d\n", ret);
			return ret;
		}
		s->writes++;
	}

        return 0;

 fail:
	free_bat(s);
	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		struct vhd_bitmap *bm = &s->bitmap_list[i];
		free(bm->map);
		free(bm->shadow);
	}
	return ret;
}

int
vhd_open (struct disk_driver *dd, const char *name, td_flag_t flags)
{
	vhd_flag_t vhd_flags = 0;

	if (flags & TD_OPEN_RDONLY)
		vhd_flags |= VHD_FLAG_OPEN_RDONLY;
	if (flags & TD_OPEN_QUIET)
		vhd_flags |= VHD_FLAG_OPEN_QUIET;
	if (flags & TD_OPEN_STRICT)
		vhd_flags |= VHD_FLAG_OPEN_STRICT;
	if (flags & TD_OPEN_QUERY)
		vhd_flags |= (VHD_FLAG_OPEN_QUERY  |
			      VHD_FLAG_OPEN_QUIET  |
			      VHD_FLAG_OPEN_RDONLY |
			      VHD_FLAG_OPEN_NO_CACHE);

	return __vhd_open(dd, name, vhd_flags);
}

int 
vhd_close(struct disk_driver *dd)
{
	int i;
	off64_t off;
	struct vhd_bitmap *bm;
	struct vhd_state  *s = (struct vhd_state *)dd->private;
	
	DBG("vhd_close\n");
	DBG("%s: QUEUED: %" PRIu64 ", COMPLETED: %" PRIu64 ", "
	    "RETURNED: %" PRIu64 "\n", s->name,
	    s->queued, s->completed, s->returned);
	DBG("WRITES: %" PRIu64 ", AVG_WRITE_SIZE: %f" PRIu64 "\n",
	    s->writes, (s->writes ? ((float)s->write_size / s->writes) : 0.0));
	DBG("READS: %" PRIu64 ", AVG_READ_SIZE: %f" PRIu64 "\n",
	    s->reads, (s->reads ? ((float)s->read_size / s->reads) : 0.0));

	/* don't write footer if tapdisk is read-only */
	if (dd->flags & TD_OPEN_RDONLY)
		goto free;
	
	/* write footer if:
	 *   - we killed it on open (opened with strict) 
	 *   - we've written data since opening
	 */
	if (test_vhd_flag(s->flags, VHD_FLAG_OPEN_STRICT) || s->writes) {
		if (s->ftr.type != HD_TYPE_FIXED) {
			off = s->next_db << VHD_SECTOR_SHIFT;
			if (lseek64(s->fd, off, SEEK_SET) == (off64_t)-1) {
				DPRINTF("ERROR: seeking footer extension.\n");
				goto free;
			}
		} else {
			off = lseek64(s->fd, 0, SEEK_END);
			if (off == (off64_t)-1) {
				DPRINTF("ERROR: seeking footer extension.\n");
				goto free;
			}
			if (lseek64(s->fd, (off64_t)(off - 512), 
				    SEEK_SET) == (off64_t)-1) {
				DPRINTF("ERROR: seeking footer extension.\n");
				goto free;
			}
		}
		if (vhd_write_hd_ftr(s->fd, &s->ftr))
			DPRINTF("ERROR: writing footer. %d\n", errno);
	}

 free:
	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		bm = &s->bitmap_list[i];
		free(bm->map);
		free(bm->shadow);
	}

	free_bat(s);

	if (fsync(s->fd) == -1)
		DPRINTF("ERROR: syncing file: %d\n", errno);
	if (close(s->fd) == -1)
		DPRINTF("ERROR: closing file: %d\n", errno);

	tp_close(&s->tp);
	
	return 0;
}

int
vhd_validate_parent(struct disk_driver *child_dd, 
		    struct disk_driver *parent_dd, td_flag_t flags)
{
	struct stat stats;
	struct vhd_state *child  = (struct vhd_state *)child_dd->private;
	struct vhd_state *parent = (struct vhd_state *)parent_dd->private;

	/* 
	 * This check removed because of cases like:
	 *   - parent VHD marked as 'hidden'
	 *   - parent VHD modified during coalesce
	 */
	/*
	if (stat(parent->name, &stats)) {
		DPRINTF("ERROR stating parent file %s\n", parent->name);
		return -errno;
	}

	if (child->hdr.prt_ts != vhd_time(stats.st_mtime)) {
		DPRINTF("ERROR: parent file has been modified since "
			"snapshot.  Child image no longer valid.\n");
		return -EINVAL;
	}
	*/

	if (uuid_compare(child->hdr.prt_uuid, parent->ftr.uuid)) {
		DPRINTF("ERROR: parent uuid has changed since "
			"snapshot.  Child image no longer valid.\n");
		return -EINVAL;
	}

	/* TODO: compare sizes */
	
	return 0;
}

static int 
macx_encode_location(char *name, char **out, int *outlen)
{
	iconv_t cd;
	int len, err;
	size_t ibl, obl;
	char *uri, *urip, *uri_utf8, *uri_utf8p, *ret;

	err     = 0;
	ret     = NULL;
	*out    = NULL;
	*outlen = 0;
	len     = strlen(name) + strlen("file://") + 1;

	uri = urip = malloc(len);
	uri_utf8 = uri_utf8p = malloc(len * 2);

	if (!uri || !uri_utf8)
		return -ENOMEM;

	cd = iconv_open("UTF-8", "ASCII");
	if (cd == (iconv_t)-1) {
		err = -errno;
		goto out;
	}

	ibl = len;
	obl = len * 2;
	sprintf(uri, "file://%s", name);

	if (iconv(cd, &urip, &ibl, &uri_utf8p, &obl) == (size_t)-1 || ibl) {
		err = (errno ? -errno : -EIO);
		goto out;
	}

	len = (len * 2) - obl;
	ret = malloc(len);
	if (!ret) {
		err = -ENOMEM;
		goto out;
	}

	memcpy(ret, uri_utf8, len);
	*outlen = len;
	*out    = ret;

 out:
	free(uri);
	free(uri_utf8);
	if (cd != (iconv_t)-1)
		iconv_close(cd);

	return err;
}

static char *
macx_decode_location(char *in, char *out, int len)
{
	iconv_t cd;
	char *name;
	size_t ibl, obl;

	name = out;
	ibl  = obl = len;

	cd = iconv_open("ASCII", "UTF-8");
	if (cd == (iconv_t)-1) 
		return NULL;

	if (iconv(cd, &in, &ibl, &out, &obl) == (size_t)-1 || ibl)
		return NULL;

	iconv_close(cd);
	*out = '\0';

	if (strstr(name, "file://") != name) {
		DPRINTF("ERROR: invalid locator name %s\n", name);
		return NULL;
	}
	name += strlen("file://");

	return strdup(name);
}

static char *
w2u_decode_location(char *in, char *out, int len)
{
	iconv_t cd;
	char *name, *tmp;
	size_t ibl, obl;

	tmp = name = out;
	ibl = obl  = len;

	cd = iconv_open("ASCII", "UTF-16");
	if (cd == (iconv_t)-1) 
		return NULL;

	if (iconv(cd, &in, &ibl, &out, &obl) == (size_t)-1 || ibl)
		return NULL;

	iconv_close(cd);
	*out = '\0';

	/* TODO: spaces */
	while (tmp++ != out)
		if (*tmp == '\\')
			*tmp = '/';

	if (strstr(name, "C:") == name || strstr(name, "c:") == name)
		name += strlen("c:");

	return strdup(name);
}

int
vhd_get_parent_id(struct disk_driver *child_dd, struct disk_id *id)
{
	struct stat stats;
	struct prt_loc *loc;
	int i, n, size, err = -EINVAL;
	char *raw, *out, *name = NULL;
	struct vhd_state *child = (struct vhd_state *)child_dd->private;

	DBG("\n");

	out = id->name = NULL;
	if (child->ftr.type != HD_TYPE_DIFF)
		return TD_NO_PARENT;

	n = sizeof(child->hdr.loc) / sizeof(struct prt_loc);
	for (i = 0; i < n && !id->name; i++) {
		raw = out = NULL;

		loc = &child->hdr.loc[i];
		if (loc->code != PLAT_CODE_MACX && 
		    loc->code != PLAT_CODE_W2KU)
			continue;

		if (lseek64(child->fd, loc->data_offset, 
			    SEEK_SET) == (off64_t)-1) {
			err = -errno;
			continue;
		}

		/* data_space *should* be in sectors, 
		 * but sometimes we find it in bytes */
		if (loc->data_space < 512)
			size = loc->data_space << VHD_SECTOR_SHIFT;
		else if (loc->data_space % 512 == 0)
			size = loc->data_space;
		else {
			err = -EINVAL;
			continue;
		}

		if (posix_memalign((void **)&raw, 512, size)) {
			err = -ENOMEM;
			continue;
		}

		if (read(child->fd, raw, size) != size) {
			err = -errno;
			goto next;
		}

		out = malloc(loc->data_len + 1);
		if (!out) {
			err = -errno;
			goto next;
		}
		
		switch(loc->code) {
		case PLAT_CODE_MACX:
			name = macx_decode_location(raw, out, loc->data_len);
			break;
		case PLAT_CODE_W2KU:
			name = w2u_decode_location(raw, out, loc->data_len);
			break;
		}

		if (name) {
			if (stat(name, &stats) == -1) {
				err    = -EINVAL;
				goto next;
			}

			id->name       = name;
			id->drivertype = DISK_TYPE_VHD;
			err            = 0;
		} else
			err            = -EINVAL;

	next:
		free(raw);
		free(out);
	}

	DBG("done: %s\n", id->name);
	return err;
}

int
vhd_get_info(struct disk_driver *dd, struct vhd_info *info)
{
	int err;
	char *buf = NULL;
	struct hd_ftr ftr;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	info->spb         = s->spb;
	info->secs        = dd->td_state->size;
	info->bat_entries = s->hdr.max_bat_size;

	if (s->ftr.type != HD_TYPE_FIXED) {
		if (posix_memalign((void **)&buf, 512, 512)) {
			err = -errno;
			goto fail;
		}
		if (lseek64(s->fd, 0, SEEK_SET) == (off64_t)-1) {
			err = -errno;
			goto fail;
		}
		if (read(s->fd, buf, 512) != 512) {
			err = (errno ? -errno : -EIO);
			goto fail;
		}
		memcpy(&ftr, buf, sizeof(struct hd_ftr));
		
		info->td_fields[TD_FIELD_HIDDEN] = (long)ftr.hidden;

		free(buf);
	} else {
		info->td_fields[TD_FIELD_HIDDEN] = (long)s->ftr.hidden;
	}

	return 0;

 fail:
	free(buf);
	return err;
}

int
vhd_get_bat(struct disk_driver *dd, struct vhd_info *info)
{
	int err;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	if (!s->bat.bat) {
		if ((err = alloc_bat(s)) != 0) {
			DPRINTF("Error allocating BAT: %d\n", err);
			return -err;
		}

		if ((err = vhd_read_bat(s->fd, s)) != 0) {
			DPRINTF("Error reading BAT: %d\n", err);
			free_bat(s);
			return -err;
		}
	}

	info->spb         = s->spb;
	info->secs        = dd->td_state->size;
	info->bat_entries = s->hdr.max_bat_size;
	info->bat         = malloc(sizeof(uint32_t) * info->bat_entries);
	if (!info->bat)
		return -ENOMEM;

	memcpy(info->bat, s->bat.bat, sizeof(uint32_t) * info->bat_entries);
	return 0;
}

int
vhd_get_header(struct disk_driver *dd, struct dd_hdr *header)
{
	struct vhd_state *s = (struct vhd_state *)dd->private;
	if (s->ftr.type == HD_TYPE_DYNAMIC || s->ftr.type == HD_TYPE_DIFF) {
		memcpy(header, &s->hdr, sizeof(struct dd_hdr));
		return 0;
	} else {
		memset(header, 0, sizeof(struct dd_hdr));
		return -EINVAL;
	}
}

void
vhd_get_footer(struct disk_driver *dd, struct hd_ftr *footer)
{
	struct vhd_state *s = (struct vhd_state *)dd->private;
	memcpy(footer, &s->ftr, sizeof(struct hd_ftr));
}

int
vhd_set_field(struct disk_driver *dd, td_field_t field, long value)
{
	off64_t end;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	switch(field) {
	case TD_FIELD_HIDDEN:
		if (value < 0 || value > 256)
			return -ERANGE;
		s->ftr.hidden = (char)value;
		break;
	default:
		return -EINVAL;
	}

	if (s->ftr.type != HD_TYPE_FIXED) {
		/* store special fields in backup footer at start of file */
		if (lseek64(s->fd, 0, SEEK_SET) == (off64_t)-1)
			return -errno;
	} else {
		if ((end = lseek64(s->fd, 0, SEEK_END)) == (off64_t)-1)
			return -errno;
		if (lseek64(s->fd, 
			    (off64_t)(end - 512), SEEK_SET) == (off64_t)-1)
			return -errno;
	}

	return vhd_write_hd_ftr(s->fd, &s->ftr);
}

/* 
 * inserts file locator between header and bat,
 * and adjusts hdr.table_offset as necessary.
 */
static int
write_locator_entry(struct vhd_state *s, int idx,
		    char *entry, int len, int type)
{
	struct prt_loc *loc;

	if (idx < 0 || idx > sizeof(s->hdr.loc) / sizeof(struct prt_loc))
		return -EINVAL;
	
	loc                  = &s->hdr.loc[idx];
	loc->code            = type;
	loc->data_len        = len;
	loc->data_space      = secs_round_up(len);
	loc->data_offset     = s->hdr.table_offset;
	s->hdr.table_offset += (loc->data_space << VHD_SECTOR_SHIFT);

	if (lseek64(s->fd, loc->data_offset, SEEK_SET) == (off64_t)-1)
		return -errno;

	if (write(s->fd, entry, len) != len)
		return (errno ? -errno : -EIO);

	return 0;
}

static int
set_parent_name(struct vhd_state *child, char *pname)
{
	char *tmp;
	iconv_t cd;
	int ret = 0;
	size_t ibl, obl;

	cd = iconv_open("UTF-16", "ASCII");
	if (cd == (iconv_t)-1)
		return -errno;

	ibl = strlen(pname);
	obl = sizeof(child->hdr.prt_name);
	tmp = child->hdr.prt_name;

	if (iconv(cd, &pname, &ibl, &tmp, &obl) == (size_t)-1 || ibl)
		ret = (errno ? -errno : -EINVAL);

	iconv_close(cd);
	return ret;
}

/*
 * set_parent may adjust hdr.table_offset.
 * call set_parent before writing the bat.
 */
static int
set_parent(struct vhd_state *child, struct vhd_state *parent, 
	   struct disk_id *parent_id, vhd_flag_t flags)
{
	struct stat stats;
	int len, err = 0, lidx = 0;
	char *loc, *file, *parent_path, *absolute_path = NULL;

	parent_path = parent_id->name;
	file = basename(parent_path); /* (using GNU, not POSIX, basename) */
	absolute_path = realpath(parent_path, NULL);

	if (!absolute_path || strcmp(file, "") == 0) {
		DPRINTF("ERROR: invalid path %s\n", parent_path);
		err = (errno ? -errno : -EINVAL);
		goto out;
	}

	if (stat(absolute_path, &stats)) {
		DPRINTF("ERROR stating %s\n", absolute_path);
		err = -errno;
		goto out;
	}

	child->hdr.prt_ts = vhd_time(stats.st_mtime);
	if (parent)
		uuid_copy(child->hdr.prt_uuid, parent->ftr.uuid);
	else {
		/* TODO: hack vhd metadata to store parent driver type */
	}

	if ((err = set_parent_name(child, file)) != 0)
		goto out;

	if (parent_path[0] != '/') {
		/* relative path */
		if ((err = macx_encode_location(parent_path, &loc, &len)) != 0)
			goto out;

		err = write_locator_entry(child, lidx++, 
					  loc, len, PLAT_CODE_MACX);
		free(loc);

		if (err)
			goto out;
	}

	/* absolute path */
	if ((err = macx_encode_location(absolute_path, &loc, &len)) != 0)
		goto out;

	err = write_locator_entry(child, lidx++, loc, len, PLAT_CODE_MACX);
	free(loc);

 out:
	free(absolute_path);
	return err;
}

int
__vhd_create(const char *name, uint64_t total_size, 
	     struct disk_id *backing_file, vhd_flag_t flags)
{
	struct hd_ftr *ftr;
	struct dd_hdr *hdr;
	struct vhd_state s;
	uint64_t size, blks;
	u32 type, *bat = NULL;
	int fd, spb, err, i, BLK_SHIFT, ret, sparse;

	sparse = test_vhd_flag(flags, VHD_FLAG_CR_SPARSE);
	BLK_SHIFT = 21;   /* 2MB blocks */

	hdr  = &s.hdr;
	ftr  = &s.ftr;
	err  = 0;
	bat  = NULL;

	blks = (total_size + ((u64)1 << BLK_SHIFT) - 1) >> BLK_SHIFT;
	size = blks << BLK_SHIFT;
	type = ((sparse) ? HD_TYPE_DYNAMIC : HD_TYPE_FIXED);
	if (sparse && backing_file)
		type = HD_TYPE_DIFF;

	s.fd = fd = open(name, 
			 O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
	if (fd < 0)
		return -errno;

	memset(ftr, 0, sizeof(struct hd_ftr));
	memcpy(ftr->cookie, HD_COOKIE, sizeof(ftr->cookie));
	ftr->features     = HD_RESERVED;
	ftr->ff_version   = HD_FF_VERSION;
	ftr->timestamp    = vhd_time(time(NULL));
	ftr->crtr_ver     = VHD_CREATOR_VERSION;
	ftr->crtr_os      = 0x00000000;
	ftr->orig_size    = size;
	ftr->curr_size    = size;
	ftr->geometry     = chs(size);
	ftr->type         = type;
	ftr->saved        = 0;
	ftr->data_offset  = ((sparse) ? VHD_SECTOR_SIZE : 0xFFFFFFFFFFFFFFFF);
	strcpy(ftr->crtr_app, "tap");
	uuid_generate(ftr->uuid);
	ftr->checksum = vhd_footer_checksum(ftr);

	if (sparse) {
		int bat_secs;

		memset(hdr, 0, sizeof(struct dd_hdr));
		memcpy(hdr->cookie, DD_COOKIE, sizeof(hdr->cookie));
		hdr->data_offset   = (u64)-1;
		hdr->table_offset  = VHD_SECTOR_SIZE * 3; /* 1 ftr + 2 hdr */
		hdr->hdr_ver       = DD_VERSION;
		hdr->block_size    = 0x00200000;
		hdr->prt_ts        = 0;
		hdr->res1          = 0;
		hdr->max_bat_size  = blks;

		if (backing_file) {
			struct vhd_state  *p = NULL;
			vhd_flag_t         oflags;
			struct td_state    tds;
			struct disk_driver parent;

			if (test_vhd_flag(flags, VHD_FLAG_CR_IGNORE_PARENT))
				goto set_parent;

			memset(&parent, 0, sizeof(struct disk_driver));
			parent.td_state = &tds;
			parent.private  = malloc(sizeof(struct vhd_state));
			if (!parent.private) {
				DPRINTF("ERROR allocating parent state\n");
				err = -ENOMEM;
				goto out;
			}
			oflags = VHD_FLAG_OPEN_RDONLY | VHD_FLAG_OPEN_NO_CACHE;
			err = __vhd_open(&parent, backing_file->name, oflags);
			if (err) {
				DPRINTF("ERROR: opening parent %s",
					backing_file->name);
				goto out;
			}

			p = (struct vhd_state *)parent.private;
			blks = (p->ftr.curr_size + ((u64)1 << BLK_SHIFT) - 1)
							   >> BLK_SHIFT;
			ftr->orig_size    = p->ftr.curr_size;
			ftr->curr_size    = p->ftr.curr_size;
			ftr->geometry     = chs(ftr->orig_size);
			ftr->checksum     = vhd_footer_checksum(ftr);
			hdr->max_bat_size = blks;

		set_parent:
			err = set_parent(&s, p, backing_file, flags);
			if (p) {
				vhd_close(&parent);
				free(p);
			}
			if (err) {
				DPRINTF("ERROR attaching to parent %s (%d)\n",
					backing_file->name, err);
				goto out;
			}
		}

		hdr->checksum = vhd_header_checksum(hdr);
#if (DEBUGGING == 1)
		debug_print_footer(ftr);
		debug_print_header(hdr);
#endif

		/* copy of footer */
		if (lseek64(fd, 0, SEEK_SET) == (off64_t)-1) {
			DPRINTF("ERROR seeking footer copy\n");
			err = -errno;
			goto out;
		}
		if ((err = vhd_write_hd_ftr(fd, ftr)))
			goto out;

		/* header */
		if (lseek64(fd, ftr->data_offset, SEEK_SET) == (off64_t)-1) {
			DPRINTF("ERROR seeking header\n");
			err = -errno;
			goto out;
		}
		if ((err = vhd_write_dd_hdr(fd, hdr)))
			goto out;

		bat_secs = secs_round_up(blks * sizeof(u32));
		bat = calloc(1, bat_secs << VHD_SECTOR_SHIFT);
		if (!bat) {
			err = -ENOMEM;
			goto out;
		}

		for (i = 0; i < blks; i++) {
			bat[i] = DD_BLK_UNUSED;
			BE32_OUT(&bat[i]);
		}

		/* bat */
		if (lseek64(fd, hdr->table_offset, SEEK_SET) == (off64_t)-1) {
			DPRINTF("ERROR seeking bat\n");
			err = -errno;
			goto out;
		}
		if (write(fd, bat, bat_secs << VHD_SECTOR_SHIFT) !=
		    bat_secs << VHD_SECTOR_SHIFT) {
			err = (errno ? -errno : -EIO);
			goto out;
		}
	} else {
		char *buf;
		ssize_t megs, mb;
		
		mb   = 1 << 20;
		megs = size >> 20;
		buf  = calloc(1, mb);
		if (!buf) {
			err = -ENOMEM;
			goto out;
		}

		for (i = 0; i < megs; i++)
			if (write(fd, buf, mb) != mb) {
				err = (errno ? -errno : -EIO);
				free(buf);
				goto out;
			}
		free(buf);
	}

	if ((err = vhd_write_hd_ftr(fd, ftr)))
		goto out;

	/* finished */
		
	DPRINTF("size: %llu, blk_size: %" PRIu64 ", blks: %" PRIu64 "\n",
		ftr->orig_size, (uint64_t)1 << BLK_SHIFT, blks);

	err = 0;

out:
	free(bat);
	close(fd);
	if (err)
		unlink(name);
	return err;
}

int
vhd_create(const char *name, uint64_t total_size, td_flag_t td_flags)
{
	vhd_flag_t vhd_flags = 0;

	if (td_flags & TD_CREATE_SPARSE)
		vhd_flags |= VHD_FLAG_CR_SPARSE;

	return __vhd_create(name, total_size, NULL, vhd_flags);
}

int
vhd_snapshot(struct disk_id *parent_id, char *child_name, td_flag_t td_flags)
{
	vhd_flag_t vhd_flags = VHD_FLAG_CR_SPARSE;

	if (td_flags & TD_CREATE_MULTITYPE)
		return -EINVAL; /* multitype snapshots not yet supported */

	return __vhd_create(child_name, 0, parent_id, vhd_flags);
}

static inline void
clear_req_list(struct vhd_req_list *list)
{
	list->head = list->tail = NULL;
}

static inline void
add_to_tail(struct vhd_req_list *list, struct vhd_request *e)
{
	if (!list->head) 
		list->head = list->tail = e;
	else 
		list->tail = list->tail->next = e;
}

static inline int
remove_from_req_list(struct vhd_req_list *list, struct vhd_request *e)
{
	struct vhd_request *i = list->head;

	if (i == e) {
		list->head = list->head->next;
		return 0;
	}

	while (i->next) {
		if (i->next == e) {
			i->next = i->next->next;
			return 0;
		}
		i = i->next;
	}

	return -EINVAL;
}

static inline void
init_tx(struct vhd_transaction *tx)
{
	memset(tx, 0, sizeof(struct vhd_transaction));
}

static inline void
add_to_transaction(struct vhd_transaction *tx, struct vhd_request *r)
{
	ASSERT(!tx->closed);

	r->tx = tx;
	tx->started++;
	add_to_tail(&tx->requests, r);
	set_vhd_flag(tx->status, VHD_FLAG_TX_LIVE);

	DBG("blk: %" PRIu64 ", lsec: %" PRIu64 ", tx: %p, "
	    "started: %d, finished: %d, status: %u\n",
	    r->lsec / SPB, r->lsec, tx,
	    tx->started, tx->finished, tx->status);
}

static inline int
transaction_completed(struct vhd_transaction *tx)
{
	return (tx->started == tx->finished);
}

static inline void
init_bat(struct vhd_state *s)
{
	s->bat.req.tx     = NULL;
	s->bat.req.next   = NULL;
	s->bat.req.error  = 0;
	s->bat.pbw_blk    = 0;
	s->bat.pbw_offset = 0;
	s->bat.status     = 0;
}

static inline void
lock_bat(struct vhd_state *s)
{
	set_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED);
}

static inline void
unlock_bat(struct vhd_state *s)
{
	clear_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED);
}

static inline int
bat_locked(struct vhd_state *s)
{
	return test_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED);
}

static inline void
init_vhd_bitmap(struct vhd_state *s, struct vhd_bitmap *bm)
{
	bm->blk    = 0;
	bm->seqno  = 0;
	bm->status = 0;
	init_tx(&bm->tx);
	clear_req_list(&bm->queue);
	clear_req_list(&bm->waiting);
	memset(bm->map, 0, s->bm_secs << VHD_SECTOR_SHIFT);
	memset(bm->shadow, 0, s->bm_secs << VHD_SECTOR_SHIFT);
	memset(&bm->req, 0, sizeof(struct vhd_request));
}

static inline struct vhd_bitmap *
get_bitmap(struct vhd_state *s, uint32_t block)
{
	int i;
	struct vhd_bitmap *bm;

	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		bm = s->bitmap[i];
		if (bm && bm->blk == block)
			return bm;
	}

	return NULL;
}

static inline void
lock_bitmap(struct vhd_bitmap *bm)
{
	set_vhd_flag(bm->status, VHD_FLAG_BM_LOCKED);
}

static inline void
unlock_bitmap(struct vhd_bitmap *bm)
{
	clear_vhd_flag(bm->status, VHD_FLAG_BM_LOCKED);
}

static inline int
bitmap_locked(struct vhd_bitmap *bm)
{
	return test_vhd_flag(bm->status, VHD_FLAG_BM_LOCKED);
}

static inline int
bitmap_valid(struct vhd_bitmap *bm)
{
	return !test_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING);
}

static inline int
bitmap_in_use(struct vhd_bitmap *bm)
{
	return (test_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING)  ||
		test_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING) ||
		test_vhd_flag(bm->tx.status, VHD_FLAG_TX_UPDATE_BAT) ||
		bm->waiting.head || bm->tx.requests.head || bm->queue.head);
}

static struct vhd_bitmap*
remove_lru_bitmap(struct vhd_state *s)
{
	int i, idx = 0;
	u64 seq = s->bm_lru;
	struct vhd_bitmap *bm, *lru = NULL;

	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		bm = s->bitmap[i];
		if (bm && bm->seqno < seq && !bitmap_locked(bm)) {
			idx = i;
			lru = bm;
			seq = lru->seqno;
		}
	}

	if (lru) {
		s->bitmap[idx] = NULL;
		ASSERT(!bitmap_in_use(lru));
	}

	return  lru;
}

static int
alloc_vhd_bitmap(struct vhd_state *s, struct vhd_bitmap **bitmap, uint32_t blk)
{
	struct vhd_bitmap *bm;
	
	*bitmap = NULL;

	if (s->bm_free_count > 0) {
		bm = s->bitmap_free[--s->bm_free_count];
	} else {
		bm = remove_lru_bitmap(s);
		if (!bm)
			return -EBUSY;
	}

	init_vhd_bitmap(s, bm);
	bm->blk = blk;
	*bitmap = bm;

	return 0;
}

static inline uint64_t
__bitmap_lru_seqno(struct vhd_state *s)
{
	int i;
	struct vhd_bitmap *bm;

	if (s->bm_lru == 0xffffffff) {
		s->bm_lru = 0;
		for (i = 0; i < VHD_CACHE_SIZE; i++) {
			bm = s->bitmap[i];
			if (bm) {
				bm->seqno >>= 1;
				if (bm->seqno > s->bm_lru)
					s->bm_lru = bm->seqno;
			}
		}
	}

	return ++s->bm_lru;
}

static inline void
touch_bitmap(struct vhd_state *s, struct vhd_bitmap *bm)
{
	bm->seqno = __bitmap_lru_seqno(s);
}

static inline void
install_bitmap(struct vhd_state *s, struct vhd_bitmap *bm)
{
	int i;
	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		if (!s->bitmap[i]) {
			touch_bitmap(s, bm);
			s->bitmap[i] = bm;
			return;
		}
	}

	ASSERT(0);
}

static inline void
free_vhd_bitmap(struct vhd_state *s, struct vhd_bitmap *bm)
{
	int i;

	for (i = 0; i < VHD_CACHE_SIZE; i++)
		if (s->bitmap[i] == bm)
			break;

	ASSERT(!bitmap_locked(bm));
	ASSERT(!bitmap_in_use(bm));
	ASSERT(i < VHD_CACHE_SIZE);

	s->bitmap[i] = NULL;
	s->bitmap_free[s->bm_free_count++] = bm;
}

static int
read_bitmap_cache(struct vhd_state *s, uint64_t sector, uint8_t op)
{
	u32 blk, sec;
	struct vhd_bitmap *bm;

	/* in fixed disks, every block is present */
	if (s->ftr.type == HD_TYPE_FIXED) 
		return VHD_BM_BIT_SET;

	blk = sector / s->spb;
	sec = sector % s->spb;

	if (blk > s->hdr.max_bat_size) {
		DPRINTF("ERROR: sec %" PRIu64 " out of range, op = %d\n", sector, op);
		return -EINVAL;
	}

	if (bat_entry(s, blk) == DD_BLK_UNUSED) {
		if (op == VHD_OP_DATA_WRITE &&
		    s->bat.pbw_blk != blk && bat_locked(s))
			return VHD_BM_BAT_LOCKED;

		return VHD_BM_BAT_CLEAR;
	}

	bm = get_bitmap(s, blk);
	if (!bm)
		return VHD_BM_NOT_CACHED;

	/* bump lru count */
	touch_bitmap(s, bm);

	if (test_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING))
		return VHD_BM_READ_PENDING;

	return ((test_bit(s, sec, bm->map)) ? 
		VHD_BM_BIT_SET : VHD_BM_BIT_CLEAR);
}

static int
read_bitmap_cache_span(struct vhd_state *s, 
		       uint64_t sector, int nr_secs, int value)
{
	int ret;
	u32 blk, sec;
	struct vhd_bitmap *bm;

	/* in fixed disks, every block is present */
	if (s->ftr.type == HD_TYPE_FIXED) 
		return nr_secs;

	sec = sector % s->spb;
	blk = sector / s->spb;
	bm  = get_bitmap(s, blk);
	
	ASSERT(bm && bitmap_valid(bm));

	for (ret = 0; sec < s->spb && ret < nr_secs; sec++, ret++)
		if (test_bit(s, sec, bm->map) != value)
			break;

	return ret;
}

static inline struct vhd_request *
alloc_vhd_request(struct vhd_state *s)
{
	struct vhd_request *req = NULL;
	
	if (s->vreq_free_count > 0) {
		req = s->vreq_free[--s->vreq_free_count];
		ASSERT(req->nr_secs == 0);
		return req;
	}

	return NULL;
}

static inline void
free_vhd_request(struct vhd_state *s, struct vhd_request *req)
{
	memset(req, 0, sizeof(struct vhd_request));
	s->vreq_free[s->vreq_free_count++] = req;
}

static inline void
aio_read(struct vhd_state *s, struct vhd_request *req, uint64_t offset)
{
	struct tiocb *tiocb = &req->tiocb;

	td_prep_read(tiocb, s->dd, s->fd, req->buf,
		     req->nr_secs << VHD_SECTOR_SHIFT, offset, (void *)req);
	td_queue_tiocb(s->dd, tiocb);

	s->queued++; 
	s->reads++; 
	s->read_size += req->nr_secs;
	TRACE(s);
}

static inline void
aio_write(struct vhd_state *s, struct vhd_request *req, uint64_t offset)
{
	struct tiocb *tiocb = &req->tiocb;

	td_prep_write(tiocb, s->dd, s->fd, req->buf,
		      req->nr_secs << VHD_SECTOR_SHIFT, offset, (void *)req);
	td_queue_tiocb(s->dd, tiocb);

	s->queued++;
	s->writes++;
	s->write_size += req->nr_secs;
	TRACE(s);
}

static inline uint64_t
reserve_new_block(struct vhd_state *s, uint32_t blk)
{
	ASSERT(!bat_locked(s) &&
	       !test_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED));

	s->bat.pbw_blk    = blk;
	s->bat.pbw_offset = s->next_db;
	lock_bat(s);

	return s->next_db;
}

static int
schedule_bat_write(struct vhd_state *s)
{
	int i;
	u32 blk;
	u64 offset;
	struct vhd_request *req;

	tp_log(&s->tp, blk, TAPPROF_IN);

	ASSERT(bat_locked(s));

	req = &s->bat.req;
	blk = s->bat.pbw_blk;
	memcpy(req->buf, &bat_entry(s, blk - (blk % 128)), 512);

	((u32 *)req->buf)[blk % 128] = s->bat.pbw_offset;

	for (i = 0; i < 128; i++)
		BE32_OUT(&((u32 *)req->buf)[i]);

	offset       = s->hdr.table_offset + (blk - (blk % 128)) * 4;
	req->nr_secs = 1;
	req->op      = VHD_OP_BAT_WRITE;

	aio_write(s, req, offset);
	set_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED);

	DBG("blk: %u, pbwo: %" PRIu64 ", table_offset: %llu\n",
	    blk, s->bat.pbw_offset, offset);

	tp_log(&s->tp, blk, TAPPROF_OUT);

	return 0;
}

static void
schedule_zero_bm_write(struct vhd_state *s, struct vhd_bitmap *bm)
{
	uint64_t offset;
	struct vhd_request *req = &s->bat.zero_req;

	offset       = s->bat.pbw_offset << VHD_SECTOR_SHIFT;
	req->op      = VHD_OP_ZERO_BM_WRITE;
	req->lsec    = s->bat.pbw_blk * s->spb;
	req->nr_secs = s->bm_secs;

	DBG("blk: %u, writing zero bitmap at %" PRIu64 "\n", 
	    s->bat.pbw_blk, offset);

	lock_bitmap(bm);
	add_to_transaction(&bm->tx, req);
	aio_write(s, req, offset);
}

static int
update_bat(struct vhd_state *s, uint32_t blk)
{
	int err;
	struct vhd_bitmap *bm;

	ASSERT(bat_entry(s, blk) == DD_BLK_UNUSED);
	
	if (bat_locked(s)) {
		ASSERT(s->bat.pbw_blk == blk);
		return 0;
	}

	/* empty bitmap could already be in
	 * cache if earlier bat update failed */
	bm = get_bitmap(s, blk);
	if (!bm) {
		/* install empty bitmap in cache */
		err = alloc_vhd_bitmap(s, &bm, blk);
		if (err) 
			return err;

		install_bitmap(s, bm);
	}

	reserve_new_block(s, blk);
	schedule_zero_bm_write(s, bm);
	set_vhd_flag(bm->tx.status, VHD_FLAG_TX_UPDATE_BAT);

	return 0;
}

#if (PREALLOCATE_BLOCKS == 1)
static int
allocate_block(struct vhd_state *s, uint32_t blk)
{
	int err, gap;
	uint64_t offset, size;
	struct vhd_bitmap *bm;

	ASSERT(bat_entry(s, blk) == DD_BLK_UNUSED);

	if (bat_locked(s)) {
		ASSERT(s->bat.pbw_blk == blk);
		if (s->bat.req.error)
			return -EBUSY;
		return 0;
	}

	gap            = 0;
	s->bat.pbw_blk = blk;
	offset         = s->next_db << VHD_SECTOR_SHIFT;

	/* data region of segment should begin on page boundary */
	if ((s->next_db + s->bm_secs) % s->spp) {
		gap = (s->spp - ((s->next_db + s->bm_secs) % s->spp));
		s->next_db += gap;
	}

	s->bat.pbw_offset = s->next_db;

	DBG("blk: %u, pbwo: %" PRIu64 "\n", blk, s->bat.pbw_offset);

	if (lseek(s->fd, offset, SEEK_SET) == (off_t)-1) {
		DBG("lseek failed: %d\n", errno);
		TAP_ERROR(errno, "lseek failed");
		return -errno;
	}

	size = ((u64)(s->spb + s->bm_secs + gap)) << VHD_SECTOR_SHIFT;
	if (size > s->zsize) {
		DPRINTF("ERROR: size: %" PRIx64 ", zsize: %x, gap: %d\n",
			size, s->zsize, gap);
		size = s->zsize;
	}

	if ((err = write(s->fd, s->zeros, size)) != size) {
		err = (err == -1 ? -errno : -EIO);
		DBG("write failed: %d\n", err);
		TAP_ERROR(err, "write failed");
		return err;
	}

	/* empty bitmap could already be in
	 * cache if earlier bat update failed */
	bm = get_bitmap(s, blk);
	if (!bm) {
		/* install empty bitmap in cache */
		err = alloc_vhd_bitmap(s, &bm, blk);
		if (err) 
			return err;

		install_bitmap(s, bm);
	}

	lock_bat(s);
	lock_bitmap(bm);
	schedule_bat_write(s);
	add_to_transaction(&bm->tx, &s->bat.req);

	return 0;
}
#endif

static int 
schedule_data_read(struct vhd_state *s, uint64_t sector,
		   int nr_secs, char *buf, uint8_t flags,
		   td_callback_t cb, int id, void *private)
{
	u64 offset;
	u32 blk = 0, sec = 0;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	tp_log(&s->tp, sector, TAPPROF_IN);

	if (s->ftr.type == HD_TYPE_FIXED) {
		offset = sector << VHD_SECTOR_SHIFT;
		goto make_request;
	}

	blk    = sector / s->spb;
	sec    = sector % s->spb;
	bm     = get_bitmap(s, blk);
	offset = bat_entry(s, blk);
	
	ASSERT(offset != DD_BLK_UNUSED);
	ASSERT(bm && bitmap_valid(bm));
	
	offset  += s->bm_secs + sec;
	offset <<= VHD_SECTOR_SHIFT;

 make_request:
	req = alloc_vhd_request(s);
	if (!req) 
		return -EBUSY;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->flags   = flags;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = VHD_OP_DATA_READ;

	aio_read(s, req, offset);

	DBG("%s: lsec: %" PRIu64 ", blk: %u, sec: %u, "
	    "nr_secs: %u, offset: %llu, flags: %u, buf: %p\n", s->name,
	    req->lsec, blk, sec, req->nr_secs, offset, req->flags, buf);

	tp_log(&s->tp, sector, TAPPROF_OUT);

	return 0;
}

static int
schedule_data_write(struct vhd_state *s, uint64_t sector,
		    int nr_secs, char *buf, uint8_t flags,
		    td_callback_t cb, int id, void *private)
{
	int err;
	u64 offset;
	u32 blk = 0, sec = 0;
	struct vhd_bitmap  *bm = NULL;
	struct vhd_request *req;

	tp_log(&s->tp, sector, TAPPROF_IN);

	if (s->ftr.type == HD_TYPE_FIXED) {
		offset = sector << VHD_SECTOR_SHIFT;
		goto make_request;
	}

	blk    = sector / s->spb;
	sec    = sector % s->spb;
	offset = bat_entry(s, blk);

	if (test_vhd_flag(flags, VHD_FLAG_REQ_UPDATE_BAT)) {
#if (PREALLOCATE_BLOCKS == 1)
		err = allocate_block(s, blk);
#else
		err = update_bat(s, blk);
#endif
		if (err)
			return err;

		offset = s->bat.pbw_offset;
	}

	offset  += s->bm_secs + sec;
	offset <<= VHD_SECTOR_SHIFT;

 make_request:
	req = alloc_vhd_request(s);
	if (!req)
		return -EBUSY;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->flags   = flags;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = VHD_OP_DATA_WRITE;

	if (test_vhd_flag(flags, VHD_FLAG_REQ_UPDATE_BITMAP)) {
		bm = get_bitmap(s, blk);
		ASSERT(bm && bitmap_valid(bm));
		lock_bitmap(bm);

		if (bm->tx.closed) {
			add_to_tail(&bm->queue, req);
			set_vhd_flag(req->flags, VHD_FLAG_REQ_QUEUED);
		} else
			add_to_transaction(&bm->tx, req);
	}

	aio_write(s, req, offset);

	DBG("%s: lsec: %" PRIu64 ", blk: %u, sec: %u, "
	    "nr_secs: %u, offset: %llu, flags: %u\n", s->name, 
	    req->lsec, blk, sec, req->nr_secs, offset, req->flags);

	tp_log(&s->tp, sector, TAPPROF_OUT);

	return 0;
}

static int 
schedule_bitmap_read(struct vhd_state *s, uint32_t blk)
{
	int err;
	u64 offset;
	struct vhd_bitmap  *bm;
	struct vhd_request *req = NULL;

	tp_log(&s->tp, blk, TAPPROF_IN);

	ASSERT(s->ftr.type != HD_TYPE_FIXED);

	offset = bat_entry(s, blk);

	ASSERT(offset != DD_BLK_UNUSED);
	ASSERT(!get_bitmap(s, blk));

	offset <<= VHD_SECTOR_SHIFT;

	err = alloc_vhd_bitmap(s, &bm, blk);
	if (err)
		return err;

	req          = &bm->req;
	req->lsec    = blk * s->spb;
	req->nr_secs = s->bm_secs;
	req->buf     = bm->map;
	req->op      = VHD_OP_BITMAP_READ;

	aio_read(s, req, offset);
	lock_bitmap(bm);
	install_bitmap(s, bm);
	set_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING);

	DBG("%s: lsec: %" PRIu64 ", blk: %u, nr_secs: %u, offset: %llu.\n",
	    s->name, req->lsec, blk, req->nr_secs, offset);

	tp_log(&s->tp, blk, TAPPROF_OUT);

	return 0;
}

static int
schedule_bitmap_write(struct vhd_state *s, uint32_t blk)
{
	u64 offset;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	tp_log(&s->tp, blk, TAPPROF_IN);

	bm     = get_bitmap(s, blk);
	offset = bat_entry(s, blk);

	ASSERT(s->ftr.type != HD_TYPE_FIXED);
	ASSERT(bm && bitmap_valid(bm) &&
	       !test_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING));

	if (offset == DD_BLK_UNUSED) {
		ASSERT(bat_locked(s) && s->bat.pbw_blk == blk);
		offset = s->bat.pbw_offset;
	}
	
	offset <<= VHD_SECTOR_SHIFT;

	req          = &bm->req;
	req->lsec    = blk * s->spb;
	req->nr_secs = s->bm_secs;
	req->buf     = bm->shadow;
	req->op      = VHD_OP_BITMAP_WRITE;

	aio_write(s, req, offset);
	lock_bitmap(bm);
	touch_bitmap(s, bm);     /* bump lru count */
	set_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING);

	DBG("%s: blk: %u, sec: %" PRIu64 ", nr_secs: %u, offset: %llu\n",
	    s->name, blk, req->lsec, req->nr_secs, offset);

	tp_log(&s->tp, blk, TAPPROF_OUT);

	return 0;
}

/* 
 * queued requests will be submitted once the bitmap
 * describing them is read and the requests are validated. 
 */
static int
__vhd_queue_request(struct vhd_state *s, uint8_t op, 
		    uint64_t sector, int nr_secs, char *buf,
		    td_callback_t cb, int id, void *private)
{
	u32 blk;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	tp_log(&s->tp, sector, TAPPROF_IN);

	ASSERT(s->ftr.type != HD_TYPE_FIXED);

	blk = sector / s->spb;
	bm  = get_bitmap(s, blk);

	ASSERT(bm && test_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING));

	req = alloc_vhd_request(s);
	if (!req)
		return -EBUSY;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = op;

	add_to_tail(&bm->waiting, req);
	lock_bitmap(bm);

	DBG("data request queued: %s: lsec: %" PRIu64 ", blk: %u nr_secs: %u, "
	    "op: %u\n", s->name, req->lsec, blk, req->nr_secs, op);

	TRACE(s);
	tp_log(&s->tp, sector, TAPPROF_OUT);
	
	return 0;
}

int
vhd_queue_read(struct disk_driver *dd, uint64_t sector, 
	       int nr_sectors, char *buf, td_callback_t cb, 
	       int id, void *private) 
{
	int rsp = 0, ret;
	uint64_t sec, end;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, sector, TAPPROF_IN);

	DBG("%s: sector: %" PRIu64 ", nb_sectors: %d (seg: %ld), buf: %p\n",
	    s->name, sector, nr_sectors, (unsigned long)private, buf);

	sec = sector;
	end = sector + nr_sectors;

	while (sec < end) {
		int n = 1, err = 0, remaining = end - sec;

		switch (read_bitmap_cache(s, sec, VHD_OP_DATA_READ)) {
		case -EINVAL:
			return cb(dd, -EINVAL, sec, remaining, id, private);
			
		case VHD_BM_BAT_CLEAR:
			n   = MIN(remaining, s->spb - (sec % s->spb));
			ret = cb(dd, BLK_NOT_ALLOCATED, sec, n, id, private);
			if (ret < 0)
				return cb(dd, ret, sec + n, 
					  remaining - n, id, private);
			else 
				rsp += ret;
			break;

		case VHD_BM_BIT_CLEAR:
			n   = read_bitmap_cache_span(s, sec, remaining, 0);
			ret = cb(dd, BLK_NOT_ALLOCATED, sec, n, id, private);
			if (ret < 0)
				return cb(dd, ret, sec + n,
					  remaining - n, id, private);
			else 
				rsp += ret;
			break;

		case VHD_BM_BIT_SET:
			n   = read_bitmap_cache_span(s, sec, remaining, 1);
			err = schedule_data_read(s, sec, n, buf, 0,
						 cb, id, private);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);
			break;

		case VHD_BM_NOT_CACHED:
			n   = MIN(remaining, s->spb - (sec % s->spb));
			err = schedule_bitmap_read(s, sec / s->spb);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);

			err = __vhd_queue_request(s, VHD_OP_DATA_READ, sec,
						  n, buf, cb, id, private);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);
			break;

		case VHD_BM_READ_PENDING:
			n   = MIN(remaining, s->spb - (sec % s->spb));
			err = __vhd_queue_request(s, VHD_OP_DATA_READ, sec,
						  n, buf, cb, id, private);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);
			break;

		case VHD_BM_BAT_LOCKED:
		default:
			ASSERT(0);
			break;
		}

		sec += n;
		buf += VHD_SECTOR_SIZE * n;
	}

	tp_log(&s->tp, sector, TAPPROF_OUT);

	return rsp;
}

int
vhd_queue_write(struct disk_driver *dd, uint64_t sector, 
		int nr_sectors, char *buf, td_callback_t cb, 
		int id, void *private) 
{
	uint64_t sec, end;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, sector, TAPPROF_IN);

	DBG("%s: sector: %" PRIu64 ", nb_sectors: %d, (seg: %ld)\n",
	    s->name, sector, nr_sectors, (unsigned long)private);

	sec = sector;
	end = sector + nr_sectors;

	while (sec < end) {
		uint8_t flags = 0;
		int n = 1, err = 0, remaining = end - sec;

		switch (read_bitmap_cache(s, sec, VHD_OP_DATA_WRITE)) {
		case -EINVAL:
			return cb(dd, -EINVAL, sec, remaining, id, private);

		case VHD_BM_BAT_LOCKED:
			return cb(dd, -EBUSY, sec, remaining, id, private);

		case VHD_BM_BAT_CLEAR:
			flags = (VHD_FLAG_REQ_UPDATE_BAT |
				 VHD_FLAG_REQ_UPDATE_BITMAP);
			n     = MIN(remaining, s->spb - (sec % s->spb));
			err   = schedule_data_write(s, sec, n, buf, 
						    flags, cb, id, private);
			if (err)
				return cb(dd, err, sec,
					  remaining, id, private);
			break;

		case VHD_BM_BIT_CLEAR:
			flags = VHD_FLAG_REQ_UPDATE_BITMAP;
			n     = read_bitmap_cache_span(s, sec, remaining, 0);
			err   = schedule_data_write(s, sec, n, buf, 
						    flags, cb, id, private);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);
			break;

		case VHD_BM_BIT_SET:
			n   = read_bitmap_cache_span(s, sec, remaining, 1);
			err = schedule_data_write(s, sec, n, buf, 0,
						  cb, id, private);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);
			break;

		case VHD_BM_NOT_CACHED:
			n   = MIN(remaining, s->spb - (sec % s->spb));
			err = schedule_bitmap_read(s, sec / s->spb);
			if (err) 
				return cb(dd, err, sec, 
					  remaining, id, private);

			err = __vhd_queue_request(s, VHD_OP_DATA_WRITE, sec,
						  n, buf, cb, id, private);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);

			break;

		case VHD_BM_READ_PENDING:
			n   = MIN(remaining, s->spb - (sec % s->spb));
			err = __vhd_queue_request(s, VHD_OP_DATA_WRITE, sec,
						  n, buf, cb, id, private);
			if (err)
				return cb(dd, err, sec, 
					  remaining, id, private);
			break;

		default:
			ASSERT(0);
			break;
		}

		sec += n;
		buf += VHD_SECTOR_SIZE * n;
	}

	tp_log(&s->tp, sector, TAPPROF_OUT);

	return 0;
}

static inline int
signal_completion(struct disk_driver *dd, struct vhd_request *list, int error)
{
	int err, rsp = 0;
	struct vhd_request *r, *next;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	r = list;
	while (r) {
		int err;

		err  = (error ? error : r->error);
		next = r->next;
		rsp += r->cb(dd, err, r->lsec, r->nr_secs, r->id, r->private);
		DBG("lsec: %" PRIu64 ", blk: %" PRIu64 ", err: %d\n", 
		    r->lsec, r->lsec / s->spb, err);
		free_vhd_request(s, r);
		r    = next;

		s->returned++;
		TRACE(s);
	}

	return rsp;
}

static int
start_new_bitmap_transaction(struct disk_driver *dd, struct vhd_bitmap *bm)
{
	int i, error = 0, rsp = 0;
	struct vhd_request *r, *next;
	struct vhd_transaction *tx;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	if (!bm->queue.head)
		return 0;

	DBG("blk: %u\n", bm->blk);

	r  = bm->queue.head;
	tx = &bm->tx;
	clear_req_list(&bm->queue);

	if (r && bat_entry(s, bm->blk) == DD_BLK_UNUSED)
		tx->error = -EIO;

	while (r) {
		next    = r->next;
		r->next = NULL;
		clear_vhd_flag(r->flags, VHD_FLAG_REQ_QUEUED);

		add_to_transaction(tx, r);
		if (test_vhd_flag(r->flags, VHD_FLAG_REQ_FINISHED)) {
			tx->finished++;
			if (!r->error) {
				u32 sec = r->lsec % s->spb;
				for (i = 0; i < r->nr_secs; i++)
					set_bit(s, sec + i, bm->shadow);
			}
		}
		r = next;
	}

	/* perhaps all the queued writes already completed? */
	if (tx->started && transaction_completed(tx))
		rsp += finish_data_transaction(dd, bm);

	return rsp;
}

static void
finish_bat_transaction(struct vhd_state *s, struct vhd_bitmap *bm)
{
	struct vhd_transaction *tx = &bm->tx;

	if (!bat_locked(s))
		return;

	if (s->bat.pbw_blk != bm->blk)
		return;

	if (!s->bat.req.error)
		goto release;

	if (!test_vhd_flag(tx->status, VHD_FLAG_TX_LIVE))
		goto release;

	tx->closed = 1;
	return;

 release:
	DBG("blk: %u\n", bm->blk);
	unlock_bat(s);
	init_bat(s);
}

static int
finish_bitmap_transaction(struct disk_driver *dd, 
			  struct vhd_bitmap *bm, int error)
{
	int map_size, rsp = 0;
	struct vhd_transaction *tx = &bm->tx;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	DBG("blk: %u, err: %d\n", bm->blk, error);
	tx->error = (tx->error ? tx->error : error);
	map_size  = s->bm_secs << VHD_SECTOR_SHIFT;

#if (PREALLOCATE_BLOCKS != 1)
	if (test_vhd_flag(tx->status, VHD_FLAG_TX_UPDATE_BAT)) {
		/* still waiting for bat write */
		ASSERT(bm->blk == s->bat.pbw_blk);
		ASSERT(test_vhd_flag(s->bat.status, 
				     VHD_FLAG_BAT_WRITE_STARTED));
		s->bat.req.tx = tx;
		return 0;
	}
#endif

	if (tx->error) {
		/* undo changes to shadow */
		memcpy(bm->shadow, bm->map, map_size);
	} else {
		/* complete atomic write */
		memcpy(bm->map, bm->shadow, map_size);
	}

	/* transaction done; signal completions */
	rsp += signal_completion(dd, tx->requests.head, tx->error);
	init_tx(tx);
	rsp += start_new_bitmap_transaction(dd, bm);

	if (!bitmap_in_use(bm))
		unlock_bitmap(bm);

	finish_bat_transaction(s, bm);

	return rsp;
}

static int
finish_data_transaction(struct disk_driver *dd, struct vhd_bitmap *bm)
{
	struct vhd_transaction *tx = &bm->tx;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	DBG("blk: %u\n", bm->blk);

	tx->closed = 1;

	if (!tx->error) {
		schedule_bitmap_write(s, bm->blk);
		return 0;
	}

	return finish_bitmap_transaction(dd, bm, 0);
}

static int
finish_bat_write(struct disk_driver *dd, struct vhd_request *req)
{
	int rsp = 0;
	struct vhd_bitmap *bm;
	struct vhd_transaction *tx;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);
	s->returned++;
	TRACE(s);

	bm = get_bitmap(s, s->bat.pbw_blk);
	
	DBG("blk %u, pbwo: %" PRIu64 ", err %d\n", 
	    s->bat.pbw_blk, s->bat.pbw_offset, req->error);
	ASSERT(bm && bitmap_valid(bm));
	ASSERT(bat_locked(s) &&
	       test_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED));
	
	tx = &bm->tx;
	ASSERT(test_vhd_flag(tx->status, VHD_FLAG_TX_LIVE));

	if (!req->error) {
		bat_entry(s, s->bat.pbw_blk) = s->bat.pbw_offset;
		s->next_db += s->spb + s->bm_secs;
#if (PREALLOCATE_BLOCKS != 1)
		/* data region of segment should begin on page boundary */
		if ((s->next_db + s->bm_secs) % s->spp)
			s->next_db += (s->spp - 
				       ((s->next_db + s->bm_secs) % s->spp));
#endif
	} else
		tx->error = req->error;

#if (PREALLOCATE_BLOCKS == 1)
	tx->finished++;
	remove_from_req_list(&tx->requests, req);
	if (transaction_completed(tx))
		finish_data_transaction(dd, bm);
#else
	clear_vhd_flag(tx->status, VHD_FLAG_TX_UPDATE_BAT);
	if (s->bat.req.tx)
		rsp += finish_bitmap_transaction(dd, bm, req->error);
#endif

	finish_bat_transaction(s, bm);

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);

	return rsp;
}

static int
finish_zero_bm_write(struct disk_driver *dd, struct vhd_request *req)
{
	u32 blk;
	int rsp = 0;
	struct vhd_bitmap *bm;
	struct vhd_transaction *tx = req->tx;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	s->returned++;
	TRACE(s);

	blk = req->lsec / s->spb;
	bm  = get_bitmap(s, blk);

	DBG("blk: %u\n", blk);
	ASSERT(bat_locked(s));
	ASSERT(s->bat.pbw_blk == blk);
	ASSERT(bm && bitmap_valid(bm) && bitmap_locked(bm));

	tx->finished++;
	remove_from_req_list(&tx->requests, req);

	if (req->error) {
		unlock_bat(s);
		init_bat(s);
		tx->error = req->error;
		clear_vhd_flag(tx->status, VHD_FLAG_TX_UPDATE_BAT);
	} else
		schedule_bat_write(s);

	if (transaction_completed(tx))
		rsp += finish_data_transaction(dd, bm);

	return rsp;
}

static int
finish_bitmap_read(struct disk_driver *dd, struct vhd_request *req)
{
	u32 blk;
	int rsp = 0;
	struct vhd_bitmap  *bm;
	struct vhd_request *r, *next;
	struct vhd_state   *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);
	s->returned++;
	TRACE(s);

	blk = req->lsec / s->spb;
	bm  = get_bitmap(s, blk);

	DBG("blk: %u\n", blk);
	ASSERT(bm && test_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING));

	r = bm->waiting.head;
	clear_req_list(&bm->waiting);
	clear_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING);
	
	if (!req->error) {
		memcpy(bm->shadow, bm->map, s->bm_secs << VHD_SECTOR_SHIFT);

		while (r) {
			struct vhd_request tmp;

			tmp  = *r;
			next =  r->next;
			free_vhd_request(s, r);

			ASSERT(tmp.op == VHD_OP_DATA_READ || 
			       tmp.op == VHD_OP_DATA_WRITE);

			if (tmp.op == VHD_OP_DATA_READ)
				rsp += vhd_queue_read(dd, tmp.lsec,
						      tmp.nr_secs, tmp.buf,
						      tmp.cb, tmp.id,
						      tmp.private);
			else if (tmp.op == VHD_OP_DATA_WRITE)
				rsp += vhd_queue_write(dd, tmp.lsec,
						       tmp.nr_secs, tmp.buf,
						       tmp.cb, tmp.id,
						       tmp.private);

			r = next;
		}
	} else {
		int err = req->error;
		unlock_bitmap(bm);
		free_vhd_bitmap(s, bm);
		return signal_completion(dd, r, err);
	}

	if (!bitmap_in_use(bm))
		unlock_bitmap(bm);

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);

	return rsp;
}

static int
finish_bitmap_write(struct disk_driver *dd, struct vhd_request *req)
{
	u32 blk;
	int rsp = 0;
	struct vhd_bitmap  *bm;
	struct vhd_state *s = (struct vhd_state *)dd->private;
	struct vhd_transaction *tx;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);
	s->returned++;
	TRACE(s);

	blk = req->lsec / s->spb;
	bm  = get_bitmap(s, blk);
	tx  = &bm->tx;

	DBG("blk: %u, started: %d, finished: %d\n", 
	    blk, tx->started, tx->finished);
	ASSERT(tx->closed);
	ASSERT(bm && bitmap_valid(bm));
	ASSERT(test_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING));

	clear_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING);

	rsp += finish_bitmap_transaction(dd, bm, req->error);

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);

	return rsp;
}

static int
finish_data_read(struct disk_driver *dd, struct vhd_request *req)
{
	int rsp;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);

	DBG("lsec %" PRIu64 ", blk: %" PRIu64 "\n", 
	    req->lsec, req->lsec / s->spb);
	rsp = signal_completion(dd, req, 0);

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);

	return rsp;
}

static int
finish_data_write(struct disk_driver *dd, struct vhd_request *req)
{
	int i, rsp = 0;
	struct vhd_state *s = (struct vhd_state *)dd->private;
	struct vhd_transaction *tx = req->tx;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);

	set_vhd_flag(req->flags, VHD_FLAG_REQ_FINISHED);

	if (tx) {
		u32 blk, sec;
		struct vhd_bitmap *bm;

		blk = req->lsec / s->spb;
		sec = req->lsec % s->spb;
		bm  = get_bitmap(s, blk);

		ASSERT(bm && bitmap_valid(bm) && bitmap_locked(bm));

		tx->finished++;

		DBG("lsec: %" PRIu64 ", blk: %" PRIu64 ", tx->started: %d, "
		    "tx->finished: %d\n", req->lsec, req->lsec / s->spb,
		    tx->started, tx->finished);

		if (!req->error)
			for (i = 0; i < req->nr_secs; i++)
				set_bit(s, sec + i, bm->shadow);

		if (transaction_completed(tx))
			rsp += finish_data_transaction(dd, bm);

	} else if (!test_vhd_flag(req->flags, VHD_FLAG_REQ_QUEUED)) {
		ASSERT(!req->next);
		DBG("lsec: %" PRIu64 ", blk: %" PRIu64 "\n", 
		    req->lsec, req->lsec / s->spb);
		rsp += signal_completion(dd, req, 0);
	}

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);

	return rsp;
}

int
vhd_complete(struct disk_driver *dd, struct tiocb *tiocb, int err)
{
	struct vhd_state *s = (struct vhd_state *)dd->private;
	struct vhd_request *req = tiocb->data;
	struct iocb *io = &tiocb->iocb;

	tp_in(&s->tp);

	s->completed++;
	TRACE(s);

	req->error = err;

	if (req->error) {
		DBG("%s: ERROR: %d: op: %u, lsec: %" PRIu64 ", "
		    "nr_secs: %u, nbytes: %lu, blk: %" PRIu64 ", "
		    "blk_offset: %u\n", s->name, req->error,
		    req->op, req->lsec, req->nr_secs, 
		    io->u.c.nbytes, req->lsec / s->spb,
		    bat_entry(s, req->lsec / s->spb));
		TAP_ERROR(req->error, "%s: aio failed: op: %u, "
			  "lsec: %" PRIu64 ", nr_secs: %u, nbytes: %lu, "
			  "blk: %" PRIu64 ", blk_offset: %u", s->name, 
			  req->op, req->lsec, req->nr_secs, 
			  io->u.c.nbytes, req->lsec / s->spb,
			  bat_entry(s, req->lsec / s->spb));
	}

	switch (req->op) {
	case VHD_OP_DATA_READ:
		finish_data_read(dd, req);
		break;

	case VHD_OP_DATA_WRITE:
		finish_data_write(dd, req);
		break;

	case VHD_OP_BITMAP_READ:
		finish_bitmap_read(dd, req);
		break;

	case VHD_OP_BITMAP_WRITE:
		finish_bitmap_write(dd, req);
		break;

	case VHD_OP_ZERO_BM_WRITE:
		finish_zero_bm_write(dd, req);
		break;

	case VHD_OP_BAT_WRITE:
		finish_bat_write(dd, req);
		break;

	default:
		ASSERT(0);
		break;
	}

	tp_out(&s->tp);

	return 0;
}

void 
vhd_debug(struct disk_driver *dd)
{
	int i;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	DBG("ALLOCATED REQUESTS: (%lu total)\n", VHD_REQS_DATA);
	for (i = 0; i < VHD_REQS_DATA; i++) {
		struct vhd_request *r = &s->vreq_list[i];
		if (r->lsec)
			DBG("%d: id: %d, err: %d, op: %d, lsec: %" PRIu64 ", "
			    "flags: %d, this: %p, next: %p, tx: %p\n", 
			    i, r->id, r->error, r->op, r->lsec, r->flags, 
			    r, r->next, r->tx);
	}

	DBG("BITMAP CACHE:\n");
	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		int qnum = 0, wnum = 0, rnum = 0;
		struct vhd_bitmap *bm = s->bitmap[i];
		struct vhd_transaction *tx;
		struct vhd_request *r;

		if (!bm)
			continue;

		tx = &bm->tx;
		r = bm->queue.head;
		while (r) {
			qnum++;
			r = r->next;
		}

		r = bm->waiting.head;
		while (r) {
			wnum++;
			r = r->next;
		}

		r = tx->requests.head;
		while (r) {
			rnum++;
			r = r->next;
		}

		DBG("%d: blk: %u, status: %u, q: %p, qnum: %d, w: %p, "
		    "wnum: %d, locked: %d, in use: %d, tx: %p, tx_error: %d, "
		    "started: %d, finished: %d, status: %u, reqs: %p, nreqs: %d\n",
		    i, bm->blk, bm->status, bm->queue.head, qnum, bm->waiting.head,
		    wnum, bitmap_locked(bm), bitmap_in_use(bm), tx, tx->error,
		    tx->started, tx->finished, tx->status, tx->requests.head, rnum);
	}

	DBG("BAT: status: %u, pbw_blk: %u, pbw_off: %" PRIu64 ", tx: %p\n",
	    s->bat.status, s->bat.pbw_blk, s->bat.pbw_offset, s->bat.req.tx);

/*
	for (i = 0; i < s->hdr.max_bat_size; i++)
		DPRINTF("%d: %u\n", i, s->bat.bat[i]);
*/
}

int
vhd_repair(struct disk_driver *dd)
{
	int err;
	off64_t off, end;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	if (s->ftr.type == HD_TYPE_FIXED) {
		if ((off = lseek64(s->fd, 0, SEEK_END)) == (off64_t)-1)
			return -errno;

		if (lseek64(s->fd, (off - 512), SEEK_CUR) == (off64_t)-1)
			return -errno;
	} else {
		off = s->next_db << VHD_SECTOR_SHIFT;
		if (lseek64(s->fd, off, SEEK_SET) == (off64_t)-1)
			return -errno;
	}

	err = vhd_write_hd_ftr(s->fd, &s->ftr);
	if (err)
		return err;

	off += sizeof(struct hd_ftr); /* 512 */
	if ((end = lseek64(s->fd, 0, SEEK_END)) == (off64_t)-1)
		return -errno;

	if (end != off)
		if (ftruncate(s->fd, off) == -1)
			return -errno;

	return 0;
}

struct tap_disk tapdisk_vhd = {
	.disk_type          = "tapdisk_vhd",
	.private_data_size  = sizeof(struct vhd_state),
	.private_iocbs      = VHD_REQS_META,
	.td_open            = vhd_open,
	.td_close           = vhd_close,
	.td_queue_read      = vhd_queue_read,
	.td_queue_write     = vhd_queue_write,
	.td_complete        = vhd_complete,
	.td_get_parent_id   = vhd_get_parent_id,
	.td_validate_parent = vhd_validate_parent,
	.td_snapshot        = vhd_snapshot,
	.td_create          = vhd_create
};
