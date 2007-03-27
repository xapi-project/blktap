/* block-vhd.c
 *
 * asynchronous vhd implementation.
 *
 * (c) 2006 Andrew Warfield and Jake Wires
 *
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

#include "tapdisk.h"
#include "vhd.h"
#include "bswap.h"
#include "profile.h"

#if 0
#define DDPRINTF(_f, _a...) DPRINTF(_f, ##_a)
#else
#define DDPRINTF(_f, _a...) ((void)0)
#endif

#if 1
#define ASSERT(_p)                                                     \
if ( !(_p) ) {                                                         \
	DPRINTF("%s:%d: FAILED ASSERTION: '%s'\n",                     \
		__FILE__, __LINE__, #_p); *(int*)0=0;                  \
}
#else
#define ASSERT(_p) ((void)0)
#endif

#define TRACE(s)                                                       \
do {                                                                   \
	if (s->wnext)                                                  \
		ASSERT(s->vreq_md_free_count < VHD_REQS_DATA);         \
	DDPRINTF("%s: %s: QUEUED: %llu, SUBMITTED: %llu, "             \
		"RETURNED: %llu, WWRITES: %llu, WREADS: %llu, "        \
		"WNEXT: %llu, DATA_ALLOCATED: %lu, "                   \
		"METADATA_ALLOCATED: %d, BBLK: %u, "                   \
		"BSTARTED: %d, BFINISHED: %d, BSTATUS: %u\n",          \
		__func__, s->name, s->queued, s->submitted,            \
		 s->returned, s->wwrites, s->wreads, s->wnext,         \
                VHD_REQS_DATA - s->vreq_free_count,                    \
		VHD_REQS_META - s->vreq_md_free_count,                 \
		s->bat.pbw_blk, s->bat.writes_started,                 \
		s->bat.writes_finished, s->bat.status);                \
} while(0)

/******AIO DEFINES******/
#define REQUEST_ASYNC_FD             1
#define MAX_AIO_REQS                 (MAX_REQUESTS * MAX_SEGMENTS_PER_REQ)

/******VHD DEFINES******/
#define VHD_CACHE_SIZE               32

#define VHD_REQS_DATA                MAX_AIO_REQS
#define VHD_REQS_META                (VHD_CACHE_SIZE + 1)
#define VHD_REQS_TOTAL               (VHD_REQS_DATA + VHD_REQS_META)

#define VHD_FLAG_OPEN_RDONLY         1
#define VHD_FLAG_OPEN_NO_CACHE       2

#define VHD_OP_BAT_WRITE             0
#define VHD_OP_DATA_READ             1
#define VHD_OP_DATA_WRITE            2
#define VHD_OP_BITMAP_READ           3
#define VHD_OP_BITMAP_WRITE          4

#define VHD_FLAG_BAT_LOCKED          1
#define VHD_FLAG_BAT_WRITE_STARTED   2
#define VHD_FLAG_BAT_RETRY           4

#define VHD_BM_BAT_LOCKED            0
#define VHD_BM_BAT_CLEAR             1
#define VHD_BM_BIT_CLEAR             2
#define VHD_BM_BIT_SET               3
#define VHD_BM_NOT_CACHED            4
#define VHD_BM_READ_PENDING          5

#define VHD_FLAG_BM_UPDATE_BAT       1
#define VHD_FLAG_BM_WRITE_PENDING    2
#define VHD_FLAG_BM_READ_PENDING     4         /* bitmap will not contain valid 
						* data until read completes */

#define VHD_REQ_TYPE_DATA            0
#define VHD_REQ_TYPE_METADATA        1

#define VHD_FLAG_REQ_UPDATE_BAT      1
#define VHD_FLAG_REQ_UPDATE_BITMAP   2
#define VHD_FLAG_REQ_ENQUEUE         4

#define VHD_FLAG_CR_SPARSE           1
#define VHD_FLAG_CR_IGNORE_PARENT    2

typedef uint8_t vhd_flag_t;

struct vhd_request {
	uint64_t              lsec;            /* logical disk sector */
	int                   nr_secs;
	char                 *buf;
	uint8_t               op;
	vhd_flag_t            flags;
	td_callback_t         cb;
	int                   id;
	void                 *private;
	struct iocb           iocb;
	struct vhd_request   *next;
};

struct vhd_req_list {
	struct vhd_request *head, *tail;
};

struct vhd_bat {
	uint32_t  *bat;
	vhd_flag_t status;
	uint32_t   pbw_blk;                    /* blk num of pending write */
	uint64_t   pbw_offset;                 /* file offset of same */
	struct vhd_request req;
	int writes_started, writes_finished;   /* number of pending write
						* requests requiring bat
						* update */
	struct vhd_req_list queued;            /* write requests to be included
						* in the next bat write */
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

	struct vhd_req_list waiting;           /* pending requests that cannot
					        * be serviced until this bitmap
					        * is read from disk */
	struct vhd_req_list waiting_writes;    /* completed write requests that
					        * cannot be returned until this
					        * bitmap is flushed to disk */
	int writes_started, writes_finished;   /* number of pending write
					        * requests included in the
					        * current bitmap transaction */

	/* serializes bitmap transactions */
	int writes_queued;                     /* number of pending write
						* requests waiting for the next
						* bitmap write transaction. */
	struct vhd_req_list queued;            /* write requests to be included
					        * in next bitmap write 
					        * transaction */
};

struct vhd_state {
	int fd;

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

	/* we keep two vhd_request freelists:
	 *   - one for servicing up to MAX_AIO_REQS data requests
	 *   - one for servicing up to VHD_CACHE_SIZE + 1 metadata requests.
	 * because there should be at most one io request pending per bitmap at
	 * any given time, the metadata freelist should always have a request
	 * available whenever a metadata read/write is necessary. */
	int                   vreq_free_count;
	struct vhd_request   *vreq_free[VHD_REQS_DATA];

	int                   vreq_md_free_count;
	struct vhd_request   *vreq_md_free[VHD_REQS_META];

	struct vhd_request    vreq_list[VHD_REQS_TOTAL];
	
	int                   iocb_queued;
	struct iocb          *iocb_queue[VHD_REQS_TOTAL];
	struct io_event       aio_events[VHD_REQS_TOTAL];
	io_context_t          aio_ctx;
	int                   poll_fd;         /* requires aio_poll support */

	char                 *name;

	/* debug info */
	struct profile_info   tp;
	uint64_t queued, submitted, returned, wwrites, wreads, wnext;
	uint64_t writes, reads, write_size, read_size;
	uint64_t submits, callback_sum, callbacks;
};

/* Helpers: */
#define BE32_IN(foo)  (*(foo)) = be32_to_cpu(*(foo))
#define BE64_IN(foo)  (*(foo)) = be64_to_cpu(*(foo))
#define BE32_OUT(foo) (*(foo)) = cpu_to_be32(*(foo))
#define BE64_OUT(foo) (*(foo)) = cpu_to_be64(*(foo))

#define MIN(a, b)                  (((a) < (b)) ? (a) : (b))

#define test_vhd_flag(word, flag)  ((word) & (flag))
#define set_vhd_flag(word, flag)   ((word) |= (flag))
#define clear_vhd_flag(word, flag) ((word) &= ~(flag))

#define bat_entry(s, blk)          ((s)->bat.bat[(blk)])

#define secs_round_up(bytes) \
              (((bytes) + (VHD_SECTOR_SIZE - 1)) >> VHD_SECTOR_SHIFT)

static inline int
test_bit (int nr, volatile void * addr)
{
	return (((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] >>
		(nr % (sizeof(unsigned long)*8))) & 1;
}

static inline void
clear_bit (int nr, volatile void * addr)
{
	((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] &=
		~(1 << (nr % (sizeof(unsigned long)*8)));
}

static inline void
set_bit (int nr, volatile void * addr)
{
	((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] |=
		(1 << (nr % (sizeof(unsigned long)*8)));
}

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

static u32
f_checksum( struct hd_ftr *f )
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

	cksm = f_checksum(f);
	DPRINTF("Checksum            : 0x%x|0x%x (%s)\n", f->checksum, cksm,
		f->checksum == cksm ? "Good!" : "Bad!" );

	uuid_unparse(f->uuid, uuid);
	DPRINTF("UUID                : %s\n", uuid);
	
	DPRINTF("Saved state         : %s\n", f->saved == 0 ? "No" : "Yes" );
}

static u32
h_checksum( struct dd_hdr *h )
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

	cksm = h_checksum(h);
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
vhd_read_hd_ftr(int fd, struct hd_ftr *ftr)
{
	char *buf;
	int err, secs;
	off_t vhd_end;

	err  = -1;
	secs = secs_round_up(sizeof(struct hd_ftr));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return err;

	memset(ftr, 0, sizeof(struct hd_ftr));

	/* Not sure if it's generally a good idea to use SEEK_END with -ve   */
	/* offsets, so do one seek to find the end of the file, and then use */
	/* SEEK_SETs when searching for the footer.                          */
	if ( (vhd_end = lseek64(fd, 0, SEEK_END)) == -1 ) {
		goto out;
	}

	/* Look for the footer 512 bytes before the end of the file. */
	if ( lseek64(fd, (off64_t)(vhd_end - 512), SEEK_SET) == -1 ) {
		goto out;
	}
	if ( read(fd, buf, 512) != 512 ) {
		goto out;
	}
	memcpy(ftr, buf, sizeof(struct hd_ftr));
	if ( memcmp(ftr->cookie, HD_COOKIE,  8) == 0 ) goto found_footer;
    
	/* According to the spec, pre-Virtual PC 2004 VHDs used a             */
	/* 511B footer.  Try that...                                          */
	memcpy(ftr, &buf[1], MIN(sizeof(struct hd_ftr), 511));
	if ( memcmp(ftr->cookie, HD_COOKIE,  8) == 0 ) goto found_footer;
    
	/* Last try.  Look for the copy of the footer at the start of image. */
	DPRINTF("NOTE: Couldn't find footer at the end of the VHD image.\n"
		"      Using backup footer from start of file.          \n"
		"      This VHD may be corrupt!\n");
	if (lseek64(fd, 0, SEEK_SET) == -1) {
		goto out;
	}
	if ( read(fd, buf, 512) != 512 ) {
		goto out;
	}
	memcpy(ftr, buf, sizeof(struct hd_ftr));
	if ( memcmp(ftr->cookie, HD_COOKIE,  8) == 0 ) goto found_footer;

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

/* 
 * Take a copy of the footer on the stack and update endianness.
 * Write it to the current position in the fd.
 */
static int
vhd_write_hd_ftr(int fd, struct hd_ftr *in_use_ftr)
{
	char *buf;
	int ret, secs;
	struct hd_ftr ftr = *in_use_ftr;

	secs = secs_round_up(sizeof(struct hd_ftr));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return -1;

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
	free(buf);

	return (ret != 512);
}

/* 
 * Take a copy of the header on the stack and update endianness.
 * Write it to the current position in the fd. 
 */
static int
vhd_write_dd_hdr(int fd, struct dd_hdr *in_use_hdr)
{
	char *buf;
	int ret, secs, i;
	struct dd_hdr hdr = *in_use_hdr;

	secs = secs_round_up(sizeof(struct dd_hdr));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return -1;

	BE64_OUT(&hdr.data_offset);
	BE64_OUT(&hdr.table_offset);
	BE32_OUT(&hdr.hdr_ver);
	BE32_OUT(&hdr.max_bat_size);	
	BE32_OUT(&hdr.block_size);
	BE32_OUT(&hdr.checksum);
	BE32_OUT(&hdr.prt_ts);

	for (i = 0; i < 8; i++) {
		BE32_IN(&hdr.loc[i].code);
		BE32_IN(&hdr.loc[i].data_space);
		BE32_IN(&hdr.loc[i].data_len);
		BE64_IN(&hdr.loc[i].data_offset);
	}

	memcpy(buf, &hdr, sizeof(struct dd_hdr));

	ret = write(fd, buf, 1024);
	free(buf);

	return (ret != 1024);
}

static int
vhd_read_dd_hdr(int fd, struct dd_hdr *hdr, u64 location)
{
	char *buf;
	int   err = -1, size, i;

	size = secs_round_up(sizeof(struct dd_hdr)) << VHD_SECTOR_SHIFT;

	if (posix_memalign((void **)&buf, 512, size))
		return err;

	if (lseek64(fd, location, SEEK_SET) == -1) {
		goto out;
	}
	if (read(fd, buf, size) != size) {
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
	int i, count = 0, secs, err;
	u32 entries  = s->hdr.max_bat_size;
	u64 location = s->hdr.table_offset;
    
	err  = -1;
	secs = secs_round_up(entries * sizeof(u32));

	if (posix_memalign((void **)&buf, 512, (secs << VHD_SECTOR_SHIFT)))
		return err;

	DPRINTF("Reading BAT at %lld, %d entries.\n", location, entries);

	if (lseek64(fd, location, SEEK_SET) == (off64_t)-1) {
		goto out;
	}
	if (read(fd, buf, secs << VHD_SECTOR_SHIFT)
	    != (secs << VHD_SECTOR_SHIFT) ) {
		goto out;
	}

	memcpy(s->bat.bat, buf, entries * sizeof(u32));
	s->next_db  = location >> VHD_SECTOR_SHIFT; /* BAT is sector aligned. */
	s->next_db += secs_round_up(sizeof(u32) * entries);

	/* ensure that data region of segment begins on page boundary */
	if ((s->next_db + s->bm_secs) % s->spp)
		s->next_db += (s->spp - ((s->next_db + s->bm_secs) % s->spp));

	DPRINTF("FirstDB: %llu\n", s->next_db);

	for (i = 0; i < entries; i++) {
		BE32_IN(&bat_entry(s, i));
		if ((bat_entry(s, i) != DD_BLK_UNUSED) && 
		    (bat_entry(s, i) > s->next_db)) {
			s->next_db = bat_entry(s, i) + s->spb + s->bm_secs;
			DDPRINTF("i: %d, bat[i]: %u, spb: %d, next: %llu\n",
				 i, bat_entry(s, i), s->spb,  s->next_db);
		}

		if (bat_entry(s, i) != DD_BLK_UNUSED) count++;
	}
    
	DPRINTF("NextDB: %llu\n", s->next_db);
	DPRINTF("Read BAT.  This vhd has %d full and %d unfilled data "
		"blocks.\n", count, entries - count);
	err = 0;

 out:
	free(buf);
	return err;
}

static int
init_aio_state(struct vhd_state *s)
{
	int i;

	/* initialize aio */
	s->aio_ctx = (io_context_t)REQUEST_ASYNC_FD;
	s->poll_fd = io_setup(VHD_REQS_TOTAL, &s->aio_ctx);

	if (s->poll_fd < 0) {
                if (s->poll_fd == -EAGAIN) {
                        DPRINTF("Couldn't setup AIO context.  If you are "
				"trying to concurrently use a large number "
				"of blktap-based disks, you may need to "
				"increase the system-wide aio request limit. "
				"(e.g. 'echo 1048576 > /proc/sys/fs/"
				"aio-max-nr')\n");
                } else {
                        DPRINTF("Couldn't get fd for AIO poll support.  This "
				"is probably because your kernel does not "
				"have the aio-poll patch applied.\n");
                }
		return s->poll_fd;
	}

	s->vreq_free_count     = VHD_REQS_DATA;
	s->vreq_md_free_count  = VHD_REQS_META;
	s->iocb_queued         = 0;

	memset(s->vreq_list,   0, sizeof(struct vhd_request) * VHD_REQS_TOTAL);
	memset(s->aio_events,  0, sizeof(struct io_event)    * VHD_REQS_TOTAL);
	
	for (i = 0; i < VHD_REQS_DATA; i++)
		s->vreq_free[i] = &s->vreq_list[i];
	for (i = 0; i < VHD_REQS_META; i++)
		s->vreq_md_free[i] = &s->vreq_list[i + VHD_REQS_DATA];

	return 0;
}

static inline void
init_fds(struct disk_driver *dd)
{
	int i;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	for(i = 0; i < MAX_IOFD; i++) 
		dd->io_fd[i] = 0;

	dd->io_fd[0] = s->poll_fd;
}

static int
__vhd_open (struct disk_driver *dd, const char *name, vhd_flag_t flags)
{
	u32 map_size;
        int fd, ret = 0, i, o_flags;
	struct td_state  *tds = dd->td_state;
	struct vhd_state *s   = (struct vhd_state *)dd->private;

	memset(s, 0, sizeof(struct vhd_state));

        DPRINTF("vhd_open: %s\n", name);

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
		DPRINTF("open(%s) with O_DIRECT\n", name);

	if (fd == -1) {
		DPRINTF("Unable to open [%s] (%d)!\n", name, -errno);
		return -errno;
	}

        /* Read the disk footer. */
        if (vhd_read_hd_ftr(fd, &s->ftr) != 0) {
                DPRINTF("Error reading VHD footer.\n");
                return -EINVAL;
        }
        debug_print_footer(&s->ftr);

        /* If this is a dynamic or differencing disk, read the dd header. */
        if ((s->ftr.type == HD_TYPE_DYNAMIC) ||
	    (s->ftr.type == HD_TYPE_DIFF)) {

                if (vhd_read_dd_hdr(fd, &s->hdr, 
				    s->ftr.data_offset) != 0) {
                        DPRINTF("Error reading VHD DD header.\n");
                        return -EINVAL;
                }

                if (s->hdr.hdr_ver != 0x00010000) {
                        DPRINTF("DANGER: unsupported hdr version! (0x%x)\n",
				 s->hdr.hdr_ver);
                        return -EINVAL;
                }
                debug_print_header(&s->hdr);

		s->spp     = getpagesize() >> VHD_SECTOR_SHIFT;
                s->spb     = s->hdr.block_size >> VHD_SECTOR_SHIFT;
		s->bm_secs = secs_round_up(s->spb >> 3);

                /* Allocate and read the Block Allocation Table. */
                s->bat.bat = malloc(s->hdr.max_bat_size * sizeof(u32));
                if (s->bat.bat == NULL) {
                        DPRINTF("Error allocating BAT.\n");
			return -ENOMEM;
                }

                if (vhd_read_bat(fd, s) != 0) {
                        DPRINTF("Error reading BAT.\n");
			free(s->bat.bat);
                        return -EINVAL;
                }

		if (test_vhd_flag(flags, VHD_FLAG_OPEN_NO_CACHE))
			goto out;

		if (posix_memalign((void **)&s->bat.req.buf, 512, 512)) {
			DPRINTF("Error allocating bat req.\n");
			free(s->bat.bat);
			return -ENOMEM;
		}

		/* Allocate bitmap cache */
		s->bm_lru        = 0;
		map_size         = s->bm_secs << VHD_SECTOR_SHIFT;
		s->bm_free_count = VHD_CACHE_SIZE;
		memset(s->bitmap_list, 0, 
		       sizeof(struct vhd_bitmap) * VHD_CACHE_SIZE);

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
	ret = init_aio_state(s);
	if (ret)
		goto fail;

	init_fds(dd);

	s->name          = strdup(name);
	s->fd            = fd;
        tds->size        = s->ftr.curr_size >> VHD_SECTOR_SHIFT;
        tds->sector_size = VHD_SECTOR_SIZE;
        tds->info        = 0;

        DPRINTF("vhd_open: done (sz:%llu, sct:%lu, inf:%u)\n",
		tds->size, tds->sector_size, tds->info);

	tp_open(&s->tp, s->name, "/tmp/vhd_log.txt", 100);

        return 0;

 fail:
	if (s->bat.bat)
		free(s->bat.bat);
	if (test_vhd_flag(flags, VHD_FLAG_OPEN_NO_CACHE))
		return ret;
	if (s->bat.req.buf)
		free(s->bat.req.buf);
	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		struct vhd_bitmap *bm = &s->bitmap_list[i];
		if (bm->map)
			free(bm->map);
		if (bm->shadow)
			free(bm->shadow);
	}
	return ret;
}

int
vhd_open (struct disk_driver *dd, const char *name, td_flag_t flags)
{
	vhd_flag_t vhd_flags = ((flags & TD_RDONLY) ? 
				VHD_FLAG_OPEN_RDONLY : 0);
	return __vhd_open(dd, name, vhd_flags);
}

int 
vhd_close(struct disk_driver *dd)
{
	int i, ret, flags;
	struct vhd_bitmap *bm;
	struct vhd_state  *s = (struct vhd_state *)dd->private;
	
        DPRINTF("vhd_close\n");
	DDPRINTF("%s: %s: QUEUED: %llu, SUBMITTED: %llu, RETURNED: %llu, "
		 "WRITES: %llu, READS: %llu, AVG_WRITE_SIZE: %f, "
		 "AVG_READ_SIZE: %f, AVG_SUBMIT_BATCH: %f, "
		 "CALLBACKS: %llu, AVG_CALLBACK_BATCH: %f\n", __func__, 
		 s->name, s->queued, s->submitted, s->returned, s->writes, 
		 s->reads, 
		 ((s->writes) ? ((float)s->write_size / s->writes) : 0.0),
		 ((s->reads) ? ((float)s->read_size / s->reads) : 0.0), 
		 ((s->submits) ? ((float)s->submitted / s->submits) : 0.0),
		 s->callbacks,
		 ((s->callbacks) ? 
		  ((float)s->callback_sum / s->callbacks) : 0.0));

	flags = fcntl(s->fd, F_GETFL);
	if (flags & O_RDWR) {
		ret = lseek64(s->fd, s->next_db << VHD_SECTOR_SHIFT, SEEK_SET);
		if (ret == (off64_t)-1) {
			DPRINTF("ERROR: seeking footer extension.\n");
		} else {
			if (vhd_write_hd_ftr(s->fd, &s->ftr))
				DPRINTF("ERROR: writing footer. %d\n", errno);
		}
	}

	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		bm = &s->bitmap_list[i];
		if (bm->map)
			free(bm->map);
		if (bm->shadow)
			free(bm->shadow);
	}
	if (s->bat.req.buf)
		free(s->bat.req.buf);
	if (s->bat.bat)
		free(s->bat.bat);

	io_destroy(s->aio_ctx);
	close(s->fd);

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

	if (stat(parent->name, &stats)) {
		DPRINTF("ERROR stating parent file %s\n", parent->name);
		return -errno;
	}

	if (child->hdr.prt_ts != vhd_time(stats.st_mtime)) {
		DPRINTF("ERROR: parent file has been modified since "
			"snapshot.  Child image no longer valid.\n");
		return -EINVAL;
	}

	if (uuid_compare(child->hdr.prt_uuid, parent->ftr.uuid)) {
		DPRINTF("ERROR: parent uuid has changed since "
			"snapshot.  Child image no longer valid.\n");
		return -EINVAL;
	}

	/* TODO: compare sizes */
	
	return 0;
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
	struct prt_loc *loc;
	int i, size, err = -EINVAL;
	char *raw, *out, *name = NULL;
	struct vhd_state *child = (struct vhd_state *)child_dd->private;

	DPRINTF("%s\n", __func__);

	out = id->name = NULL;
	if (child->ftr.type != HD_TYPE_DIFF)
		return TD_NO_PARENT;

	for (i = 0; i < 8 && !id->name; i++) {
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
			id->name       = name;
			id->drivertype = DISK_TYPE_VHD;
			err            = 0;
		} else
			err            = -EINVAL;

	next:
		free(raw);
		free(out);
	}

	DPRINTF("%s: done: %s\n", __func__, id->name);
	return err;
}

/*
 * set_parent may adjust hdr.table_offset.
 * call set_parent before writing the bat.
 */
int
set_parent(struct vhd_state *child, struct vhd_state *parent, 
	   struct disk_id *parent_id, vhd_flag_t flags)
{
	off64_t offset;
	int err = 0, len;
	struct stat stats;
	struct prt_loc *loc;
	iconv_t cd = (iconv_t)-1;
	size_t inbytesleft, outbytesleft;
	char *file, *parent_path, *absolute_path = NULL, *tmp;
	char *uri = NULL, *urip, *uri_utf8 = NULL, *uri_utf8p;

	parent_path = parent_id->name;
	file = basename(parent_path); /* (using GNU, not POSIX, basename) */
	absolute_path = realpath(parent_path, NULL);

	if (!absolute_path || strcmp(file, "") == 0) {
		DPRINTF("ERROR: invalid path %s\n", parent_path);
		err = -1;
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

	cd = iconv_open("UTF-16", "ASCII");
	if (cd == (iconv_t)-1) {
		DPRINTF("ERROR creating character encoder context\n");
		err = -errno;
		goto out;
	}
	inbytesleft  = strlen(file);
	outbytesleft = sizeof(child->hdr.prt_name);
	tmp          = child->hdr.prt_name;
	if (iconv(cd, &file, &inbytesleft, &tmp,
		  &outbytesleft) == (size_t)-1 || inbytesleft) {
		DPRINTF("ERROR encoding parent file name %s\n", file);
		err = -errno;
		goto out;
	}
	iconv_close(cd);

	/* absolute locator */
	len = strlen(absolute_path) + strlen("file://") + 1;
	uri = urip = malloc(len);
	uri_utf8 = uri_utf8p = malloc(len * 2);
	if (!uri || !uri_utf8) {
		DPRINTF("ERROR allocating uri\n");
		err = -ENOMEM;
		goto out;
	}
	sprintf(uri, "file://%s", absolute_path);

	cd = iconv_open("UTF-8", "ASCII");
	if (cd == (iconv_t)-1) {
		DPRINTF("ERROR creating character encoder context\n");
		err = -errno;
		goto out;
	}
	inbytesleft  = len;
	outbytesleft = len * 2;
	if (iconv(cd, &urip, &inbytesleft, &uri_utf8p, 
		  &outbytesleft) == (size_t)-1 || inbytesleft) {
		DPRINTF("ERROR encoding uri %s\n", uri);
		err = -errno;
		goto out;
	}

	len             = (2 * len) - outbytesleft;
	loc             = &child->hdr.loc[0];
	loc->code       = PLAT_CODE_MACX;
	loc->data_space = secs_round_up(len);
	loc->data_len   = len;

	/* insert file locator between header and bat */
	offset = child->hdr.table_offset;
	child->hdr.table_offset += (loc->data_space << VHD_SECTOR_SHIFT);
	loc->data_offset = offset;

	if (lseek64(child->fd, offset, SEEK_SET) == (off64_t)-1) {
		DPRINTF("ERROR seeking to file locator\n");
		err = -errno;
		goto out;
	}
	if (write(child->fd, uri_utf8, len) != len) {
		DPRINTF("ERROR writing file locator\n");
		err = -errno;
		goto out;
	}

	/* TODO: relative locator */

	err = 0;

 out:
	if (cd != (iconv_t)-1)
		iconv_close(cd);
	free(uri);
	free(uri_utf8);
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
	err  = -1;
	bat  = NULL;

	blks = (total_size + ((u64)1 << BLK_SHIFT) - 1) >> BLK_SHIFT;
	size = blks << BLK_SHIFT;
	type = ((sparse) ? HD_TYPE_DYNAMIC : HD_TYPE_FIXED);
	if (sparse && backing_file)
		type = HD_TYPE_DIFF;
		
	DPRINTF("%s: total_size: %llu, size: %llu, blk_size: %llu, "
		"blks: %llu\n",	__func__, total_size, size, 
		(uint64_t)1 << BLK_SHIFT, blks);

	s.fd = fd = open(name, 
			 O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
	if (fd < 0)
		return err;

	memset(ftr, 0, sizeof(struct hd_ftr));
	memcpy(ftr->cookie, HD_COOKIE, sizeof(ftr->cookie));
	ftr->features     = HD_RESERVED;
	ftr->ff_version   = HD_FF_VERSION;
	ftr->timestamp    = vhd_time(time(NULL));
	ftr->crtr_ver     = 0x00000001;
	ftr->crtr_os      = 0x00000000;
	ftr->orig_size    = size;
	ftr->curr_size    = size;
	ftr->geometry     = chs(size);
	ftr->type         = type;
	ftr->saved        = 0;
	ftr->data_offset  = ((sparse) ? VHD_SECTOR_SIZE : 0xFFFFFFFFFFFFFFFF);
	strcpy(ftr->crtr_app, "tap");
	uuid_generate(ftr->uuid);
	ftr->checksum = f_checksum(ftr);

	if (sparse) {
		int bat_secs;

		memset(hdr, 0, sizeof(struct dd_hdr));
		memcpy(hdr->cookie, DD_COOKIE, sizeof(hdr->cookie));
		hdr->data_offset   = (u64)-1;
		hdr->table_offset  = VHD_SECTOR_SIZE * 3; /* 1 ftr + 2 hdr */
		hdr->hdr_ver       = DD_VERSION;
		hdr->max_bat_size  = blks;
		hdr->block_size    = 0x00200000;
		hdr->prt_ts        = 0;
		hdr->res1          = 0;

		if (backing_file) {
			struct vhd_state  *p = NULL;
			vhd_flag_t         oflags;
			struct td_state    tds;
			struct disk_driver parent;

			if (test_vhd_flag(flags, VHD_FLAG_CR_IGNORE_PARENT))
				goto set_parent;

			parent.td_state = &tds;
			parent.private  = malloc(sizeof(struct vhd_state));
			if (!parent.private) {
				DPRINTF("ERROR allocating parent state\n");
				return -ENOMEM;
			}
			oflags = VHD_FLAG_OPEN_RDONLY | VHD_FLAG_OPEN_NO_CACHE;
			if ((ret = __vhd_open(&parent, backing_file->name, 
					      oflags)) != 0) {
				DPRINTF("ERROR: %s is not a valid VHD file.",
					backing_file->name);
				return ret;
			}
			p = (struct vhd_state *)parent.private;
			if (ftr->curr_size != p->ftr.curr_size) {
				DPRINTF("ERROR: child and parent image sizes "
					"do not match (c: %llu, p: %llu).\n",
					ftr->curr_size, p->ftr.curr_size);
				return -EINVAL;
			}

		set_parent:
			if ((ret = set_parent(&s, p, 
					      backing_file, flags)) != 0) {
				DPRINTF("ERROR attaching to parent %s (%d)\n",
					backing_file->name, ret);
				if (p)
					vhd_close(&parent);
				return ret;
			}
			if (p)
				vhd_close(&parent);
		}

		hdr->checksum = h_checksum(hdr);
		debug_print_footer(ftr);
		debug_print_header(hdr);

		/* copy of footer */
		if (lseek64(fd, 0, SEEK_SET) == (off64_t)-1) {
			DPRINTF("ERROR seeking footer copy\n");
			goto out;
		}
		if (vhd_write_hd_ftr(fd, ftr))
			goto out;

		/* header */
		if (lseek64(fd, ftr->data_offset, SEEK_SET) == (off64_t)-1) {
			DPRINTF("ERROR seeking header\n");
			goto out;
		}
		if (vhd_write_dd_hdr(fd, hdr))
			goto out;

		bat_secs = secs_round_up(blks * sizeof(u32));
		bat = calloc(1, bat_secs << VHD_SECTOR_SHIFT);
		if (!bat)
			goto out;

		for (i = 0; i < blks; i++) {
			bat[i] = DD_BLK_UNUSED;
			BE32_OUT(&bat[i]);
		}

		/* bat */
		if (lseek64(fd, hdr->table_offset, SEEK_SET) == (off64_t)-1) {
			DPRINTF("ERROR seeking bat\n");
			goto out;
		}
		if (write(fd, bat, bat_secs << VHD_SECTOR_SHIFT) !=
		    bat_secs << VHD_SECTOR_SHIFT)
			goto out;
	} else {
		char buf[4096];
		memset(buf, 0, 4096);

		for (i = 0; i < size; i += 4096) 
			if (write(fd, buf, 4096) != 4096) 
				goto out;
	}

	if (vhd_write_hd_ftr(fd, ftr))
		goto out;

	/* finished */
	DPRINTF("%s: done\n", __func__);
	err = 0;

out:
	free(bat);
	close(fd);
	return err;
}

/*
 * for now, vhd_create will only snapshot vhd images
 */
int
vhd_create(const char *name, uint64_t total_size, 
	   const char *backing_file, int sparse)
{
	struct disk_id id, *idp = NULL;
	vhd_flag_t flags = ((sparse) ? VHD_FLAG_CR_SPARSE : 0);

	if (backing_file) {
		id.name = (char *)backing_file;
		idp     = &id;
	}

	return __vhd_create(name, total_size, idp, flags);
}

/*
 * vhd_snapshot supports snapshotting arbitrary image types
 */
int
vhd_snapshot(struct disk_id *parent_id, 
	     char *child_name, uint64_t size, td_flag_t td_flags)
{
	vhd_flag_t vhd_flags = VHD_FLAG_CR_SPARSE;

	if (td_flags & TD_MULTITYPE_CP)
		vhd_flags |= VHD_FLAG_CR_IGNORE_PARENT;

	return __vhd_create(child_name, size, parent_id, vhd_flags);
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

static inline void
init_vhd_bitmap(struct vhd_state *s, struct vhd_bitmap *bm)
{
	bm->blk             = 0;
	bm->seqno           = 0;
	bm->status          = 0;
	bm->writes_queued   = 0;
	bm->writes_started  = 0;
	bm->writes_finished = 0;
	clear_req_list(&bm->waiting);
	clear_req_list(&bm->waiting_writes);
	clear_req_list(&bm->queued);
	memset(bm->map, 0, s->bm_secs << VHD_SECTOR_SHIFT);
	memset(bm->shadow, 0, s->bm_secs << VHD_SECTOR_SHIFT);
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
		bm->waiting.head || bm->waiting_writes.head          || 
		bm->queued.head  || bm->writes_started || bm->writes_queued);
}

static struct vhd_bitmap*
remove_lru_bitmap(struct vhd_state *s)
{
	int i, idx = 0;
	u64 seq = s->bm_lru;
	struct vhd_bitmap *bm, *lru = NULL;

	for (i = 0; i < VHD_CACHE_SIZE; i++) {
		bm = s->bitmap[i];
		if (bm && bm->seqno < seq && !bitmap_in_use(bm)) {
			idx = i;
			lru = bm;
			seq = lru->seqno;
		}
	}

	if (lru) 
		s->bitmap[idx] = NULL;

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
		DPRINTF("ERROR: read out of range.\n");
		return -EINVAL;
	}

	if (bat_entry(s, blk) == DD_BLK_UNUSED) {
		if (op == VHD_OP_DATA_WRITE &&
		    test_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED))
			return VHD_BM_BAT_LOCKED;

		return VHD_BM_BAT_CLEAR;
	}

	/* no need to check bitmap for dynamic disks */
	if (s->ftr.type == HD_TYPE_DYNAMIC)
		return VHD_BM_BIT_SET;

	bm = get_bitmap(s, blk);
	if (!bm)
		return VHD_BM_NOT_CACHED;

	/* bump lru count */
	touch_bitmap(s, bm);

	if (test_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING))
		return VHD_BM_READ_PENDING;

	return ((test_bit(sec, (void *)bm->map)) ? 
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

	/* no need to check bitmap for dynamic disks */
	if (s->ftr.type == HD_TYPE_DYNAMIC)
		return MIN(nr_secs, s->spb - sec);

	blk = sector / s->spb;
	bm  = get_bitmap(s, blk);
	
	ASSERT(bm && bitmap_valid(bm));

	for (ret = 0; sec < s->spb && ret < nr_secs; sec++, ret++)
		if (test_bit(sec, (void *)bm->map) != value)
			break;

	return ret;
}

static struct vhd_request *
__alloc_vhd_request(struct vhd_state *s, uint8_t type)
{
	struct vhd_request *req = NULL;

	if (type == VHD_REQ_TYPE_METADATA) {
		ASSERT(s->vreq_md_free_count);
		req = s->vreq_md_free[--s->vreq_md_free_count];
	} else if (s->vreq_free_count > 0)
		req = s->vreq_free[--s->vreq_free_count];
	else
		DPRINTF("ERROR: %s: -ENOMEM\n", __func__);

	if (req) 
		ASSERT(req->nr_secs == 0);

	return req;
}

static inline struct vhd_request *
alloc_vhd_request(struct vhd_state *s)
{
	return __alloc_vhd_request(s, VHD_REQ_TYPE_DATA);
}

static void
__free_vhd_request(struct vhd_state *s, struct vhd_request *req, uint8_t type)
{
	memset(req, 0, sizeof(struct vhd_request));
	if (type == VHD_REQ_TYPE_METADATA)
		s->vreq_md_free[s->vreq_md_free_count++] = req;
	else
		s->vreq_free[s->vreq_free_count++] = req;
}

static inline void
free_vhd_request(struct vhd_state *s, struct vhd_request *req)
{
	__free_vhd_request(s, req, VHD_REQ_TYPE_DATA);
}

static inline void
aio_read(struct vhd_state *s, struct vhd_request *req, uint64_t offset)
{
	struct iocb *io = &req->iocb;
	io_prep_pread(io, s->fd, req->buf, 
		      req->nr_secs << VHD_SECTOR_SHIFT, offset);
	io->data = (void *)req;
	s->iocb_queue[s->iocb_queued++] = io;

	s->queued++; 
	s->reads++; 
	s->read_size += req->nr_secs;
	TRACE(s);
}

static inline void
aio_write(struct vhd_state *s, struct vhd_request *req, uint64_t offset)
{
	struct iocb *io = &req->iocb;
	io_prep_pwrite(io, s->fd, req->buf,
		       req->nr_secs << VHD_SECTOR_SHIFT, offset);
	io->data = (void *)req;
	s->iocb_queue[s->iocb_queued++] = io;

	s->queued++;
	s->writes++;
	s->write_size += req->nr_secs;
	TRACE(s);
}

static inline uint64_t
reserve_new_block(struct vhd_state *s, uint32_t blk)
{
	ASSERT(!test_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED) &&
	       !test_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED));

	s->bat.pbw_blk         = blk;
	s->bat.pbw_offset      = s->next_db;
	s->bat.writes_started  = 0;
	s->bat.writes_finished = 0;
	set_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED);

	return s->next_db;
}

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
	
	if (s->ftr.type == HD_TYPE_DIFF) {
		ASSERT(offset != DD_BLK_UNUSED);
		ASSERT(bm && bitmap_valid(bm));
	}
	
	offset  += s->bm_secs + sec;
	offset <<= VHD_SECTOR_SHIFT;

 make_request:
	req = alloc_vhd_request(s);
	if (!req) 
		return -ENOMEM;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->flags   = flags;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = VHD_OP_DATA_READ;

	aio_read(s, req, offset);

	DDPRINTF("data read scheduled: %s: lsec: %llu, blk: %u, sec: %u, "
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
		int err;
		ASSERT(bat_entry(s, blk) == DD_BLK_UNUSED);

		/* install empty bitmap in cache */
		err = alloc_vhd_bitmap(s, &bm, blk);
		if (err) 
			return err;

		install_bitmap(s, bm);

		if (test_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED)) {
			ASSERT(s->bat.pbw_blk == blk);
			offset = s->bat.pbw_offset;
		} else
			offset = reserve_new_block(s, blk);
	}

	bm = get_bitmap(s, blk);

	if (s->ftr.type == HD_TYPE_DIFF ||
	    test_vhd_flag(flags, VHD_FLAG_REQ_UPDATE_BAT)) {
		ASSERT(offset != DD_BLK_UNUSED);
		ASSERT(bm && bitmap_valid(bm));
	}

	offset  += s->bm_secs + sec;
	offset <<= VHD_SECTOR_SHIFT;

 make_request:
	req = alloc_vhd_request(s);
	if (!req)
		return -ENOMEM;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->flags   = flags;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = VHD_OP_DATA_WRITE;

	if (test_vhd_flag(flags, VHD_FLAG_REQ_UPDATE_BITMAP)) {
		if (test_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING)) {
			/* bitmap write transaction already started. 
			 * this bitmap modification will be handled after
			 * the completion of the current transaction. */
			set_vhd_flag(req->flags, VHD_FLAG_REQ_ENQUEUE);
			++bm->writes_queued;
		} else {
			/* include this bitmap modification in the current
			 * write transaction. */
			++bm->writes_started;
		}
	}
	if (test_vhd_flag(flags, VHD_FLAG_REQ_UPDATE_BAT))
		++s->bat.writes_started;

	aio_write(s, req, offset);

	DDPRINTF("data write scheduled: %s: lsec: %llu, blk: %u, sec: %u, "
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

	req = __alloc_vhd_request(s, VHD_REQ_TYPE_METADATA);
	
	req->lsec    = blk * s->spb;
	req->nr_secs = s->bm_secs;
	req->buf     = bm->map;
	req->op      = VHD_OP_BITMAP_READ;

	aio_read(s, req, offset);
	set_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING);
	install_bitmap(s, bm);

	DDPRINTF("bitmap read scheduled: %s: lsec: %llu, blk: %u, "
		 "nr_secs: %u, offset: %llu.\n", 
		 s->name, req->lsec, blk, req->nr_secs, offset);

	tp_log(&s->tp, blk, TAPPROF_OUT);

	return 0;
}

static int
schedule_bitmap_write(struct vhd_state *s, uint32_t blk,
		      vhd_flag_t flags, struct vhd_request *wait_list)
{
	u64 offset;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	tp_log(&s->tp, blk, TAPPROF_IN);

	ASSERT(s->ftr.type != HD_TYPE_FIXED);

	bm = get_bitmap(s, blk);

	if (test_vhd_flag(flags, VHD_FLAG_BM_UPDATE_BAT)) {
		ASSERT(test_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED));
		ASSERT(!test_vhd_flag(s->bat.status, 
				      VHD_FLAG_BAT_WRITE_STARTED));
		ASSERT(s->bat.pbw_blk == blk);
		offset = s->bat.pbw_offset;
	} else
		offset = bat_entry(s, blk);

	ASSERT(offset != DD_BLK_UNUSED);
	ASSERT(bm && bitmap_valid(bm) &&
	       !test_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING));
	
	offset <<= VHD_SECTOR_SHIFT;

	req = __alloc_vhd_request(s, VHD_REQ_TYPE_METADATA);

	req->lsec    = blk * s->spb;
	req->nr_secs = s->bm_secs;
	req->buf     = bm->shadow;
	req->op      = VHD_OP_BITMAP_WRITE;
	req->next    = wait_list;

	aio_write(s, req, offset);
	set_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING);
	touch_bitmap(s, bm);     /* bump lru count */

	DDPRINTF("bitmap write scheduled: %s: lsec: %llu, blk: %u, "
		 "nr_secs: %u, offset: %llu, waiters: %p\n", s->name,
		 req->lsec, blk, req->nr_secs, offset, req->next);

	tp_log(&s->tp, blk, TAPPROF_OUT);

	return 0;
}

static int
schedule_bat_write(struct vhd_state *s, 
		   uint32_t blk, struct vhd_request *wait_list)
{
	int i;
	u64 offset;
	struct vhd_request *req;

	tp_log(&s->tp, blk, TAPPROF_IN);

	ASSERT(blk == s->bat.pbw_blk);
	ASSERT(test_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED));

	req = &s->bat.req;

	if (!test_vhd_flag(s->bat.status, VHD_FLAG_BAT_RETRY)) {
		memcpy(req->buf, &bat_entry(s, blk - (blk % 128)), 512);
		((u32 *)req->buf)[blk % 128] = s->bat.pbw_offset;

		for (i = 0; i < 128; i++)
			BE32_OUT(&((u32 *)req->buf)[i]);
	}

	offset       = s->hdr.table_offset + (blk - (blk % 128)) * 4;
	req->nr_secs = 1;
	req->op      = VHD_OP_BAT_WRITE;
	req->next    = wait_list;

	aio_write(s, req, offset);
	set_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED);

	DDPRINTF("bat write scheduled: %s, blk: %u, offset: %llu, "
		 "waiters: %p\n", s->name, blk, offset, req->next);

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
		return -ENOMEM;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = op;

	add_to_tail(&bm->waiting, req);

	DDPRINTF("data request queued: %s: lsec: %llu, blk: %u nr_secs: %u, "
		 "op: %u\n", s->name, req->lsec, blk, req->nr_secs, op);

	s->wreads++;
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

	DDPRINTF("%s: %s: sector: %llu, nb_sectors: %d (seg: %d), buf: %p\n",
		 __func__, s->name, sector, nr_sectors, (int)private, buf);

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
			if (ret == -EBUSY)
				return cb(dd, -EBUSY, sec + n, 
					  remaining - n, id, private);
			else 
				rsp += ret;
			break;

		case VHD_BM_BIT_CLEAR:
			n   = read_bitmap_cache_span(s, sec, remaining, 0);
			ret = cb(dd, BLK_NOT_ALLOCATED, sec, n, id, private);
			if (ret == -EBUSY)
				return cb(dd, -EBUSY, sec + n, 
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

	DDPRINTF("%s: %s: sector: %llu, nb_sectors: %d, (seg: %d)\n",
		 __func__, s->name, sector, nr_sectors, (int)private);

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

static int
finish_data_read(struct disk_driver *dd, struct vhd_request *req, int err)
{
	struct vhd_state *s = (struct vhd_state *)dd->private;
	int rsp = req->cb(dd, err, req->lsec, req->nr_secs,
			  req->id, req->private);

	tp_log(&s->tp, req->lsec, TAPPROF_IN);
	s->returned++;
	TRACE(s);
	tp_log(&s->tp, req->lsec, TAPPROF_OUT);

	free_vhd_request(s, req);
	return rsp;
}

static int
finish_data_write(struct disk_driver *dd, struct vhd_request *req, int err)
{
	int rsp = 0;
	struct vhd_request *r;
	struct vhd_state   *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);

	if (s->ftr.type == HD_TYPE_FIXED)
		goto out;

	if (err && test_vhd_flag(req->flags, VHD_FLAG_REQ_UPDATE_BAT))
		++s->bat.writes_finished;

	if (test_vhd_flag(req->flags, VHD_FLAG_REQ_UPDATE_BITMAP)) {
		u32 blk, sec;
		struct vhd_bitmap *bm;

		blk = req->lsec / s->spb;
		sec = req->lsec % s->spb;
		bm  = get_bitmap(s, blk);

		ASSERT(bm && bitmap_valid(bm));

		if (test_vhd_flag(req->flags, VHD_FLAG_REQ_ENQUEUE)) {
			--bm->writes_queued;

			/* if this request is not included in the current 
			 * bitmap write transaction and it failed, do not
			 * worry about updating the bitmap. */
			if (err) 
				goto out;

			if (test_vhd_flag(bm->status, 
					  VHD_FLAG_BM_WRITE_PENDING)) {
				/* this request is not part of current bitmap
				 * write transaction.  its bitmap modifications
				 * will be included in the next transaction. */
				add_to_tail(&bm->queued, req);

				s->wnext++;
				TRACE(s);

				return 0;
			} else {
				/* the previous bitmap transaction ended while
				 * this request was being satisfied.  add this
				 * request to current bitmap transaction. */
				++bm->writes_started;
				clear_vhd_flag(req->flags, 
					       VHD_FLAG_REQ_ENQUEUE);
			}
		}

		/* this data write is part of the current bitmap write
		 * transaction.  update the im-memory bitmap and start
		 * disk writeback if no other data writes in this
		 * transaction are pending. */
		if (!err) {
			/* we ignore bitmaps for dynamic disks, so it is 
			 * sufficient merely to write zeros upon a block
			 * allocation.  but we must update the bitmap
			 * properly for differencing disks. */
			if (s->ftr.type == HD_TYPE_DIFF) {
				int i;
				for (i = 0; i < req->nr_secs; i++)
					set_bit(sec + i, (void *)bm->shadow);
			}

			add_to_tail(&bm->waiting_writes, req);
			
			s->wwrites++;
			TRACE(s);
		}
		
		/* NB: the last request of a bitmap write transaction must
		 * begin bitmap writeback, even the request itself failed. */
		++bm->writes_finished;
		if (bm->writes_finished == bm->writes_started) {
			vhd_flag_t flags = 0;
			if (test_vhd_flag(req->flags, VHD_FLAG_REQ_UPDATE_BAT))
				set_vhd_flag(flags, VHD_FLAG_BM_UPDATE_BAT);

			r = bm->waiting_writes.head;
			bm->writes_started  = 0;
			bm->writes_finished = 0;
			clear_req_list(&bm->waiting_writes);
			schedule_bitmap_write(s, blk, flags, r);
		}
	}

 out:
	tp_log(&s->tp, req->lsec, TAPPROF_OUT);
	/* the completion of 
	 * failed writes and writes that did not modify the bitmap
	 * can be signalled immediately */
	if (err || !test_vhd_flag(req->flags, VHD_FLAG_REQ_UPDATE_BITMAP)) {
		rsp += req->cb(dd, err, req->lsec, 
			       req->nr_secs, req->id, req->private);
		free_vhd_request(s, req);

		s->returned++;
		TRACE(s);
	}

	return rsp;
}

static int
finish_bitmap_read(struct disk_driver *dd, struct vhd_request *req, int err)
{
	int rsp = 0;
	u32 blk, sec;
	struct vhd_bitmap  *bm;
	struct vhd_request *r, *next;
	struct vhd_state   *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);

	blk = req->lsec / s->spb;
	sec = req->lsec % s->spb;
	bm  = get_bitmap(s, blk);

	ASSERT(bm && test_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING));

	r = bm->waiting.head;
	clear_req_list(&bm->waiting);
	clear_vhd_flag(bm->status, VHD_FLAG_BM_READ_PENDING);
	
	if (!err) {
		/* success. submit any waiting requests. */
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
			
			s->wreads--;
			TRACE(s);
		}
	} else {
		/* error.  fail any waiting requests. */
		while (r) {
			rsp += r->cb(dd, err, r->lsec, 
				     r->nr_secs, r->id, r->private);

			next = r->next;
			free_vhd_request(s, r);
			r = next;

			s->returned++;
			s->wreads--;
			TRACE(s);
		}

		/* remove invalid bitmap from cache */
		free_vhd_bitmap(s, bm);
	}

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);
	s->returned++;
	TRACE(s);

	__free_vhd_request(s, req, VHD_REQ_TYPE_METADATA);

	return rsp;
}

static int
finish_bitmap_write(struct disk_driver *dd, struct vhd_request *req, int err)
{
	u32 blk;
	int rsp = 0, map_size;
	struct vhd_bitmap  *bm;
	struct vhd_request *r, *next;
	struct vhd_req_list bm_list, bat_list;
	struct vhd_state   *s = (struct vhd_state *)dd->private;

	tp_log(&s->tp, req->lsec, TAPPROF_IN);

	blk      = req->lsec / s->spb;
	bm       = get_bitmap(s, blk);
	map_size = s->bm_secs << VHD_SECTOR_SHIFT;

	ASSERT(bm && bitmap_valid(bm));
	ASSERT(test_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING));
	ASSERT(!bm->writes_started &&     /* no new requests should be added */
	       !bm->writes_finished);     /* while a transaction is pending. */

	clear_vhd_flag(bm->status, VHD_FLAG_BM_WRITE_PENDING);

	if (err) {
		/* undo changes to shadow */
		memcpy(bm->shadow, bm->map, map_size);
	} else {
		/* complete atomic write */
		memcpy(bm->map, bm->shadow, map_size);
	}

	/* handle waiting write requests */
	r = req->next;
	clear_req_list(&bat_list);
	while (r) {
		next = r->next;
		r->next = NULL;

		if (test_vhd_flag(r->flags, VHD_FLAG_REQ_UPDATE_BAT))
			++s->bat.writes_finished;

		/* must update BAT before signalling completion */
		if (!err && bat_entry(s, blk) == DD_BLK_UNUSED) {
			if (s->bat.pbw_blk != blk)
				err = -EIO;
			else if (test_vhd_flag(s->bat.status,
					       VHD_FLAG_BAT_WRITE_STARTED))
				add_to_tail(&s->bat.queued, r);
			else 
				add_to_tail(&bat_list, r);
		}

		/* we're done.  signal completion */
		if (err || bat_entry(s, blk) != DD_BLK_UNUSED) {
			rsp += r->cb(dd, err, r->lsec, 
				     r->nr_secs, r->id, r->private);
			free_vhd_request(s, r);
			
			s->returned++;
			s->wwrites--;
			TRACE(s);
		}

		r = next;
	}

	/* start BAT write if needed */
	if (bat_list.head)
		schedule_bat_write(s, blk, bat_list.head);

	/* start new transaction if more requests are queued */
	r = bm->queued.head;
	clear_req_list(&bm_list);
	clear_req_list(&bm->queued);
	while (r) {
		int i;
		u32 sec = r->lsec % s->spb;
		
		ASSERT((r->lsec / s->spb) == blk);

		for (i = 0; i < r->nr_secs; i++)
			set_bit(sec + i, (void *)bm->shadow);

		add_to_tail(&bm_list, r);
		r = r->next;

		s->wwrites++;
		s->wnext--;
		TRACE(s);
	}

	if (bm_list.head) 
		schedule_bitmap_write(s, blk, 0, bm_list.head);

	/* data writes failed, and no pending
	 * writes need to update bat, so unlock it. */
	if (s->bat.writes_started == s->bat.writes_finished &&
	    !test_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED)) {
		s->bat.writes_started  = 0;
		s->bat.writes_finished = 0;
		clear_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED);
	}

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);
	s->returned++;
	TRACE(s);

	__free_vhd_request(s, req, VHD_REQ_TYPE_METADATA);

	return rsp;
}

static int
finish_bat_write(struct disk_driver *dd, struct vhd_request *req, int err)
{
	int rsp = 0, retry = 0;
	struct vhd_request *r, *next, *tail;
	struct vhd_state   *s = (struct vhd_state *)dd->private;

	ASSERT(test_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED) &&
	       test_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED));

	tp_log(&s->tp, req->lsec, TAPPROF_IN);

	if (!err) {
		/* success: update in-memory bat */
		bat_entry(s, s->bat.pbw_blk) = s->bat.pbw_offset;
		s->next_db += s->spb + s->bm_secs;

		/* data region of segment should begin on page boundary */
		if ((s->next_db + s->bm_secs) % s->spp)
			s->next_db += (s->spp - 
				       ((s->next_db + s->bm_secs) % s->spp));
	} else if (s->bat.queued.head) {
		/* error and more requests waiting: retry */
		if (req->next) {
			/* splice current requests and queued requests */
			tail = req->next;
			while (tail->next)
				tail = tail->next;

			r = req->next;
			tail->next = s->bat.queued.head;
		} else
			r = s->bat.queued.head;
		
		retry = 1;
		req->next = NULL;
		clear_req_list(&s->bat.queued);
		schedule_bat_write(s, r->lsec / s->spb, r);
	}

	r = req->next;
	while (r) {
		next = r->next;
		rsp += r->cb(dd, err, r->lsec, r->nr_secs, r->id, r->private);
		free_vhd_request(s, r);
		r = next;

		if (!r && !err) {
			r = s->bat.queued.head;
			clear_req_list(&s->bat.queued);
		}

		s->returned++;
		s->wwrites--;
		TRACE(s);
	}

	if (retry) 
		set_vhd_flag(s->bat.status, VHD_FLAG_BAT_RETRY);
	else {
		s->bat.pbw_blk         = 0;
		s->bat.pbw_offset      = 0;
		s->bat.writes_started  = 0;
		s->bat.writes_finished = 0;
		clear_vhd_flag(s->bat.status, VHD_FLAG_BAT_RETRY);
		clear_vhd_flag(s->bat.status, VHD_FLAG_BAT_LOCKED);
	}
	clear_vhd_flag(s->bat.status, VHD_FLAG_BAT_WRITE_STARTED);

	tp_log(&s->tp, req->lsec, TAPPROF_OUT);
	s->returned++;
	TRACE(s);

	return rsp;
}

/* 
 * all requests on bitmap waitlists have either not yet been submitted 
 * (bm->waiting list), or have already completed (bm->waiting_writes and
 * bm->queued lists).  no waitlists contain in-flight data requests 
 * (i.e., data requests in iocb_queue).  thus, failed bitmap read/write
 * requests will never disturb in-flight data requests, and failed data
 * requests will never be on any waitlists. 
 */
static inline int
fail_vhd_request(struct disk_driver *dd, struct vhd_request *r, int err)
{
	ASSERT(!r->next);
	DPRINTF("ERROR: %s to %llu failed: %d\n", 
		(r->op == VHD_OP_DATA_READ || r->op == VHD_OP_BITMAP_READ) ?
		"read" : "write", r->lsec, err);

	switch(r->op) {
	case VHD_OP_DATA_READ:
		return finish_data_read(dd, r, err);

	case VHD_OP_DATA_WRITE:
		return finish_data_write(dd, r, err);

	case VHD_OP_BITMAP_READ:
		return finish_bitmap_read(dd, r, err);

	case VHD_OP_BITMAP_WRITE:
		return finish_bitmap_write(dd, r, err);

	case VHD_OP_BAT_WRITE:
		return finish_bat_write(dd, r, err);

	default:
		ASSERT(0);
		return 0;
	}
}

int
vhd_submit(struct disk_driver *dd)
{
	int ret, err = 0, rsp = 0;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	if (!s->iocb_queued)
		return 0;

	tp_in(&s->tp);

        DDPRINTF("%s: %s: submitting %d\n", __func__, s->name, s->iocb_queued);
	ret = io_submit(s->aio_ctx, s->iocb_queued, s->iocb_queue);

	s->submits++;
	s->submitted += s->iocb_queued;
	TRACE(s);

	/* io_submit returns an error, or the number of iocbs submitted. */
	if (ret < 0) {
		err = ret;
		ret = 0;
	} else if (ret < s->iocb_queued)
		err = -EIO;

	if (err) {
		int i;
		struct iocb *io;
		struct vhd_request *req;
		
		for (i = ret; i < s->iocb_queued; i++) {
			io   = s->iocb_queue[i];
			req  = (struct vhd_request *)io->data;
			rsp += fail_vhd_request(dd, req, err);
		}
	}

	s->iocb_queued = 0;

	tp_out(&s->tp);

	return rsp;
}

int
vhd_do_callbacks(struct disk_driver *dd, int sid)
{
	struct io_event   *ep;
	struct vhd_bitmap *bm;
	int ret, nr_iocbs, rsp = 0;
	struct vhd_state *s = (struct vhd_state *)dd->private;

	if (sid > MAX_IOFD)
		return 1;

	tp_in(&s->tp);

	nr_iocbs = s->iocb_queued;

	/* non-blocking test for completed io */
	ret = io_getevents(s->aio_ctx, 0, VHD_REQS_TOTAL, s->aio_events, NULL);

	s->callbacks++;
	s->callback_sum += ret;
	TRACE(s);

	for (ep = s->aio_events; ret-- > 0; ep++) {
		int err;
		struct iocb *io = ep->obj;
		struct vhd_request *req = (struct vhd_request *)io->data;

		err = (ep->res == io->u.c.nbytes) ? 0 : -EIO;

		if (err)
			DDPRINTF("%s: %s: op: %u, lsec: %llu, nr_secs: %u, "
				 "err: %d\n", __func__, s->name, req->op, 
				 req->lsec, req->nr_secs, err);

		switch (req->op) {
		case VHD_OP_DATA_READ:
			rsp += finish_data_read(dd, req, err);
			break;

		case VHD_OP_DATA_WRITE:
			rsp += finish_data_write(dd, req, err);
			break;

		case VHD_OP_BITMAP_READ:
			rsp += finish_bitmap_read(dd, req, err);
			break;

		case VHD_OP_BITMAP_WRITE:
			rsp += finish_bitmap_write(dd, req, err);
			break;

		case VHD_OP_BAT_WRITE:
			rsp += finish_bat_write(dd, req, err);
			break;
			
		default:
			ASSERT(0);
			break;
		}
	}

	if (s->iocb_queued != nr_iocbs) {
		/* additional requests were queued.  submit them. */
		DDPRINTF("%s: %s: more requests enqueued; submitting\n", 
			 __func__, s->name);
		vhd_submit(dd);
	}

	tp_out(&s->tp);

	return rsp;
}

struct tap_disk tapdisk_vhd = {
	.disk_type          = "tapdisk_vhd",
	.private_data_size  = sizeof(struct vhd_state),
	.td_open            = vhd_open,
	.td_queue_read      = vhd_queue_read,
	.td_queue_write     = vhd_queue_write,
	.td_submit          = vhd_submit,
	.td_close           = vhd_close,
	.td_do_callbacks    = vhd_do_callbacks,
	.td_get_parent_id   = vhd_get_parent_id,
	.td_validate_parent = vhd_validate_parent,
	.td_snapshot        = vhd_snapshot
};
