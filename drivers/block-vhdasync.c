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

#include "tapdisk.h"
#include "vhd.h"
#include "bswap.h"

struct vhd_request {
	uint64_t              lsec;            /* logical disk sector */
	int                   nr_secs;
	char                 *buf;
	u8_t                  op;
	u8_t                  flags;
	td_callback_t         cb;
	int                   id;
	void                 *private;
	struct iocb           iocb;
	struct vhd_request   *next;
}

struct vhd_bitmap {
	unsigned long *map;                    /* map should only be modified
					        * in finish_bitmap_write */
	unsigned long *shadow;                 /* in-memory bitmap changes are 
					        * made to shadow and copied to
					        * map only after having been
					        * flushed to disk */

	struct vhd_request *waiting;           /* requests that cannot be
					        * serviced until this bitmap is
					        * read from disk */
	struct vhd_request *waiting_writes;    /* write requests that cannot be
					        * returned until this bitmap is
					        * flushed to disk */
	int writes_started, writes_finished;   /* number of write requests
					        * dependent upon the current
					        * bitmap write transaction. */

	/* serializes bitmap transactions */
	u8_t write_pending;                    /* true if bitmap is currently 
						* being flushed */
	struct vhd_request *queued;            /* write requests to be included
					        * in next bitmap write 
					        * transaction */
}

struct vhd_state {
	int fd;
	int poll_pipe[2];                      /* dummy fd for polling on */

        /* VHD stuff */
        struct hd_ftr  ftr;
        struct dd_hdr  hdr;
        u32            spb;                    /* sectors per block */
        u32           *bat;
        u32            next_db;                /* pointer to the next 
					        * (unallocated) datablock */

	struct vhd_bitmap  *bitmap[];
	u32                 bm_size;           /* size of bitmap in bytes, 
						* padded to sector boundary. */

	/* we keep two vhd_request freelists: 
	 *   - one for servicing up to MAX_AIO_REQS data requests
	 *   - one for servicing up to max_blk_size metadata requests.
	 * because there should be at most one io request pending per
	 * bitmap at any given time, the metadata freelist should always
	 * have a request available whenever a metadata read/write is 
	 * necessary. */

	int                 vreq_free_count;   
	struct vhd_request  vreq_list[MAX_AIO_REQS];
	struct vhd_request *vreq_free[MAX_AIO_REQS];

	int                 md_max_vhd_reqs;
	int                 md_vreq_free_count;
	struct vhd_request  md_vreq_list[];
	struct vhd_request *md_vreq_free[];

	int                 total_vreqs;
	
	int                 iocb_queued;
	struct iocb        *iocb_queue[];
	struct io_event     aio_events[];
	io_context_t        aio_ctx;
	int                 poll_fd;           /* requires aio_poll support */
};

/* Helpers: */
#define BE32_IN(foo) (*(foo)) = be32_to_cpu(*(foo))
#define BE64_IN(foo) (*(foo)) = be64_to_cpu(*(foo))
#define BE32_OUT(foo) (*(foo)) = cpu_to_be32(*(foo))
#define BE64_OUT(foo) (*(foo)) = cpu_to_be64(*(foo))

#define add_to_tail(h, e) ((h) ? ((h)->next = (e)) : ((h) = (e)))

static inline int
test_bit ( int nr, volatile void * addr)
{
	return (((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] >>
		(nr % (sizeof(unsigned long)*8))) & 1;
}

static inline void
clear_bit ( int nr, volatile void * addr)
{
	((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] &=
		~(1 << (nr % (sizeof(unsigned long)*8) ) );
}

static inline void
set_bit ( int nr, volatile void * addr)
{
	((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] |=
		(1 << (nr % (sizeof(unsigned long)*8) ) );
}

/* Debug print functions: */


/* Stringify the VHD timestamp for printing.                              */
/* As with ctime_r, target must be >=26 bytes.                            */
/* TODO: Verify this is correct.                                          */
size_t 
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

	DPRINTF("Creator OS          : ");
	if ( f->crtr_os == HD_CR_OS_WINDOWS ) {
		DPRINTF("Windows\n");
	} else if ( f->crtr_os == HD_CR_OS_MACINTOSH ) {
		DPRINTF("Macintosh\n");
	} else {
		DPRINTF("Unknown! (0x%x)\n", f->crtr_os);
	}

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

	DPRINTF("VHD Footer Summary:\n-------------------\n");
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
}

/* End of debug print functions. */

static int
vhd_read_hd_ftr(int fd, struct hd_ftr *ftr)
{
	off_t vhd_end;

	memset(ftr, 0, sizeof(struct hd_ftr));

	/* Not sure if it's generally a good idea to use SEEK_END with -ve   */
	/* offsets, so do one seek to find the end of the file, and then use */
	/* SEEK_SETs when searching for the footer.                          */
	if ( (vhd_end = lseek(fd, 0, SEEK_END)) == -1 ) {
		return -1;
	}

	/* Look for the footer 512 bytes before the end of the file. */
	if ( lseek64(fd, vhd_end - 512, SEEK_SET) == -1 ) {
		return -1;
	}
	if ( read(fd, ftr, 512) != 512 ) {
		return -1;
	}
	if ( memcmp(ftr->cookie, HD_COOKIE,  8) == 0 ) goto found_footer;
    
	/* According to the spec, pre-Virtual PC 2004 VHDs used a             */
	/* 511B footer.  Try that...                                          */
	if ( lseek64(fd, vhd_end - 511, SEEK_SET) == -1 ) {
		return -1;
	}
	if ( read(fd, ftr, 511 != 511) ) {
		return -1;
	}
	if ( memcmp(ftr->cookie, HD_COOKIE,  8) == 0 ) goto found_footer;
    
	/* Last try.  Look for the copy of the footer at the start of image. */
	DPRINTF("NOTE: Couldn't find footer at the end of the VHD image.\n"
		"      Using backup footer from start of file.          \n"
		"      This VHD may be corrupt!\n");
	if (lseek(fd, 0, SEEK_SET) == -1) {
		return -1;
	}
	if ( read(fd, ftr, 512) != 512 ) {
		return -1;
	}
	if ( memcmp(ftr->cookie, HD_COOKIE,  8) == 0 ) goto found_footer;
	
	DPRINTF("error reading footer.\n");
	return -1;

 found_footer:

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
    
	return 0;
}

/* Take a copy of the footer on the stack and update endianness.
 * Write it to the current position in the fd.
 */
static int
vhd_write_hd_ftr(int fd, struct hd_ftr *in_use_ftr)
{
	struct hd_ftr ftr = *in_use_ftr;

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
	
	return (write(fd, &ftr, sizeof(struct hd_ftr)));
}

static int
vhd_read_dd_hdr(int fd, struct dd_hdr *hdr, u64 location)
{
	if ( lseek64(fd, location, SEEK_SET) == -1 ) {
		return -1;
	}
	if ( read(fd, hdr, sizeof(struct dd_hdr)) != sizeof(struct dd_hdr) ) {
		return -1;
	}
	if ( memcmp(hdr->cookie, DD_COOKIE,  8) != 0 ) 
		return -1;
	
	BE64_IN(&hdr->data_offset);
	BE64_IN(&hdr->table_offset);
	BE32_IN(&hdr->hdr_ver);
	BE32_IN(&hdr->max_bat_size);
	BE32_IN(&hdr->block_size);
	BE32_IN(&hdr->checksum);
	BE32_IN(&hdr->prt_ts);
	
	return 0;
}

static int
vhd_read_bat(int fd, struct vhd_state *s)
{
	int i, count = 0;
	u32 entries  = s->hdr.max_bat_size;
	u64 location = s->hdr.table_offset;
    
	DPRINTF("Reading BAT at %lld, %d entries.\n", location, entries);

	if ( lseek64(fd, location, SEEK_SET) == -1 ) {
		return -1;
	}
	if ( read(fd, s->bat, sizeof(u32) * entries)
	     != (sizeof(u32) * entries) ) {
		return -1;
	}
    
	s->next_db = location >> SECTOR_SHIFT; /* BAT is sector aligned. */
	s->next_db += ((sizeof(u32)*entries) + (512-1)) >> SECTOR_SHIFT;
	DPRINTF("FirstDB: %d\n", s->next_db);

	for (i = 0; i < entries; i++) {
		BE32_IN(&s->bat[i]);
		if ((s->bat[i] != DD_BLK_UNUSED) && (s->bat[i] > s->next_db)) {
			s->next_db = s->bat[i] + s->spb + 1 /* bitmap */;
			DPRINTF("i: %d, bat[i]: %u, spb: %d, next: %u\n",
				i, s->bat[i], s->spb,  s->next_db);
		}

		if (s->bat[i] != DD_BLK_UNUSED) count++;
	}
    
	DPRINTF("NextDB: %d\n", s->next_db);
	/* Test -- the vhd file i have seems to have an extra data block. */
#if 0
	{
		char bitmap[512];
		lseek(fd, s->next_db << SECTOR_SHIFT, SEEK_SET);
		read(fd, bitmap, 512);
		DPRINTF("SCANNING EXTRA BITMAP:\n");
		for (i = 0; i < 512; i++)
			if (bitmap[i] != 0)
				DPRINTF("bitmap[%d] is not zero\n", i);
	}
#endif
	DPRINTF("Read BAT.  This vhd has %d full and %d unfilled data "
		"blocks.\n", count, entries - count);
    
    return 0;
}


/*Get Image size, secsize*/
int
vhdsync_open (struct td_state *s, const char *name)
{
        int fd, ret = 0;
	struct vhdsync_state *prv = (struct vhdsync_state *)s->private;

        DPRINTF("vhdsync_open: %s\n", name);

        /* set up a pipe so that we can hand back a poll fd that won't fire.*/
	ret = pipe(prv->poll_pipe);
	if (ret != 0)
		return (0 - errno);

        prv->bat = NULL;

        /* Read the disk footer. */
        fd = open(name, O_RDWR | O_LARGEFILE);
        if ( fd < 1 ) {
                DPRINTF("Couldn't open %s\n", name);
                return -errno;
        }

        if ( vhd_read_hd_ftr(fd, &prv->ftr) != 0 ) {
                DPRINTF("Error reading VHD footer.\n");
                return -errno;
        }
        debug_print_footer(&prv->ftr);

        /* If this is a dynamic or differencing disk, read the dd header. */
        if (prv->ftr.type == HD_TYPE_DIFF)
                DPRINTF("WARNING: Differencing disks not really supported.\n");

        if ( (prv->ftr.type == HD_TYPE_DYNAMIC) ||
             (prv->ftr.type == HD_TYPE_DIFF) ) {

                if ( vhd_read_dd_hdr(fd, &prv->hdr, 
                                     prv->ftr.data_offset) != 0) {
                        DPRINTF("Error reading VHD DD header.\n");
                        return -errno;
                }

                if (prv->hdr.hdr_ver != 0x00010000) {
                        DPRINTF("DANGER: unsupported hdr version! (0x%x)\n", 
                               prv->hdr.hdr_ver);
                        return -errno;
                }
                debug_print_header(&prv->hdr);

                prv->spb = prv->hdr.block_size / VHD_SECTOR_SIZE;

                /* Allocate and read the Block Allocation Table. */
                prv->bat = malloc( prv->hdr.max_bat_size * sizeof(u32) );
                if ( prv->bat == NULL ) {
                        DPRINTF("Error allocating BAT.\n"); 
                }

                if ( vhd_read_bat(fd, prv) != 0) {
                        DPRINTF("Error reading BAT.\n");
                        return -errno;
                }

        }

	prv->fd = fd;
        s->size = prv->ftr.curr_size >> SECTOR_SHIFT;
        s->sector_size = VHD_SECTOR_SIZE;
        s->info = 0;

        DPRINTF("vhdsync_open: done (sz:%llu, sct:%lu, inf:%u)\n",
                s->size, s->sector_size, s->info);

        return 0;
}

/* Return negative value on error, zero if the sector doesn't exist in this 
 * file, and 1 on success. On success, put the offset in offset. 
 * should_allocate indicates whether a sector should be created if it doesn't
 * exist.
 */
static int
get_sector_offset(struct vhdsync_state *prv, uint64_t sector, 
		  uint64_t *offset, int should_allocate)
{
/* TODO: fix the bitmap size calculation! */

        int ret, i;
        u32 blk, sct;
        u32 bitmap[128];
        u64 db_start;

        blk = sector / prv->spb;
        sct = sector % prv->spb;

        *offset = 0;

        if (blk > prv->hdr.max_bat_size) {
                DPRINTF("ERROR: read out of range.\n");
                return(-EINVAL);
        }
        
        /* Lookup data block in the BAT. */
        if ((db_start = (u64)prv->bat[blk]) == DD_BLK_UNUSED) {
                if (should_allocate) {
                        int new_ftr_pos = prv->next_db + prv->spb + 1;
                        /* push footer forward */
                        ret = lseek(prv->fd, new_ftr_pos << SECTOR_SHIFT,
                                    SEEK_SET);
                        if (ret == (off_t)-1) {
                                DPRINTF("ERROR: seeking footer extension.\n");
                                return -errno;
                        }
                        ret = vhd_write_hd_ftr(prv->fd, &prv->ftr);
                        if (ret !=  sizeof(struct hd_ftr)) {
                                DPRINTF("ERROR: writing footer. %d\n", errno);
                                return -errno;
                        }
                        
                        /* write empty bitmap */
                        memset(bitmap, 0, 512);
                        ret = lseek(prv->fd, prv->next_db << SECTOR_SHIFT,
                                    SEEK_SET);
                        if (ret == (off_t)-1) {
                                DPRINTF("ERROR: seeking bitmap extension.\n");
                                return -errno;
                        }
                        ret = write(prv->fd, bitmap, 512);
                        if (ret !=  512) {
                                DPRINTF("ERROR: writing bitmap. %d,%d\n", 
                                        ret, errno);
                                return -errno;
                        }

                        /* update bat */
                        /* Do this as a sector-sized write. */
                        prv->bat[blk] = prv->next_db;
                        memcpy(bitmap, &prv->bat[blk-(blk%128)], 512);
                        for (i=0; i<128; i++)
                                BE32_OUT(&bitmap[i]);
                        ret = lseek(prv->fd, 
                                    prv->hdr.table_offset + (blk-(blk%128))*4,
                                    SEEK_SET);
                        if (ret == (off_t)-1) {
                                DPRINTF("ERROR: seeking bat update.\n");
                                return -errno;
                        }
                        ret = write(prv->fd, bitmap, 512);
                        if (ret !=  512) {
                                DPRINTF("ERROR: updating bat. %d\n", errno);
                                return -errno;
                        }

                        prv->next_db = new_ftr_pos;
                        db_start = (u64)prv->bat[blk];

                } else {
                        return 0;
                }
        }

        db_start <<= SECTOR_SHIFT;

        /* Read the DB bitmap. */
        /* TODO: add a cache here. */
        ret = lseek(prv->fd, db_start, SEEK_SET);
        if (ret == (off_t)-1) {
                DPRINTF("ERROR: seeking data block.\n");
                return -errno;
        }

        ret = read(prv->fd, bitmap, 512);
        if (ret !=  512) {
                DPRINTF("ERROR: reading block bitmap. %d\n", errno);
                return -errno;
        }

        if ((!test_bit(sct, (void *)bitmap)) && (!should_allocate) ) {
                return 0;
        } else {
                if (!test_bit(sct, (void *)bitmap) && should_allocate) {
                        set_bit(sct, (void *)bitmap);
                        ret = lseek(prv->fd, db_start, SEEK_SET);
                        if (ret == (off_t)-1) {
                                DPRINTF("ERROR: seeking data block.\n");
                                return -errno;
                        }
                        
                        ret = write(prv->fd, bitmap, 512);
                        if (ret !=  512) {
                                DPRINTF("ERROR: writing bitmap. %d\n", errno);
                                return -errno;
                        }
                }

                *offset =  db_start + 512/* bitmap*/ + (sct * 512);
                return 1;
        }
}

static int
init_aio_state(struct td_state *tds)
{
	int i;
	struct vhd_state *s = (struct vhd_state *)tds->private;

	s->md_vreq_list = NULL;
	s->iocb_queue   = NULL;
	s->aio_events   = NULL;

	s->vreq_free_count = MAX_AIO_REQS;

	s->md_max_vhd_reqs    = s->hdr.max_bat_size + 1;
	s->md_vreq_free_count = s->md_max_vhd_reqs;

	s->total_vreqs = s->md_max_vhd_reqs + MAX_AIO_REQS;

	/* initialize aio */
	s->aio_ctx = (io_context_t)REQUEST_ASYNC_FD;
	s->poll_fd = io_setup(s->max_vhd_reqs, &s->aio_ctx);

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

	for (i = 0; i < MAX_AIO_REQS; i++)
		s->vreq_free[i] = &s->vreq_list[i];

	s->md_vreq_list = calloc(s->md_max_vhd_reqs, 
				 sizeof(struct vhd_request));
	if (!s->md_vreq_list)
		goto no_mem;
	for (i = 0; i < s->md_max_vhd_reqs; i++)
		s->md_vreq_free[i] = &s->md_vreq_list[i];

	s->iocb_queued = 0;
	s->iocb_queue  = calloc(s->total_vreqs, sizeof(struct iocb *));
	if (!s->iocb_queue)
		goto no_mem;

	s->aio_events = calloc(s->total_vreqs, sizeof(struct aio_event));
	if (!s->aio_events)
		goto no_mem;

	s->bitmap = calloc(s->hdr.max_bat_size, sizeof(struct vhd_bitmap *));
	if (!s->bitmap)
		goto no_mem;

	return 0;

 no_mem:
	if (s->md_vreq_list)
		free(s->md_vreq_list);
	if (s->iocb_queue)
		free(s->iocb_queue);
	if (s->aio_events)
		free(s->aio_events);
	return -ENOMEM;
}

static struct vhd_request *
__alloc_vhd_request(struct vhd_state *s, u8_t type)
{
	struct vhd_request *req = NULL;

	if (type == VHD_REQ_METADATA) {
		ASSERT(s->md_vhdreq_free);
		req = s->md_vhdreq_free[--s->md_vhdreq_free_count];
	} else {
		if (s->vhdreq_free_count > 0)
			req = s->vhdreq_free[--s->vhdreq_free_count];
	}

	if (req)
		memset(req, 0, sizeof(struct vhd_request));

	return req;
}

static inline struct vhd_request *
alloc_vhd_request(struct vhd_state *s)
{
	return __alloc_vhd_request(s, VHD_REQ_DATA);
}

static void
__free_vhd_request(struct vhd_state *s, struct vhd_request *req, u8_t type)
{
	if (type == VHD_REQ_METADATA)
		s->md_vhdreq_free[s->md_vhdreq_free_count++] = req;
	else
		s->vhdreq_free[s->vhdreq_free_count++] = req;
}

static inline void
free_vhd_request(struct vhd_state *s, struct vhd_request *req)
{
	__free_vhd_request(s, req, VHD_REQ_DATA);
}

static int
read_bitmap_cache(struct vhd_state *s, uint64_t sector)
{
	u32 blk, sec;
	struct vhd_bitmap *bm;

	blk = sector / s->spb;
	sec = sector % s->spb;

	if (blk > s->hdr.max_bat_size) {
		DPRINTF("ERROR: read out of range.\n");
		return -EINVAL;
	}

	if (s->bat[blk] == DD_BLK_UNUSED)
		return VHD_BM_BAT_CLEAR;

	bm = s->bitmap[blk];
	if (!bm)
		return VHD_BM_BITMAP_NOT_CACHED;

	if (!bm->map)
		return VHD_BM_BITMAP_READ_PENDING;

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

	blk = sector / s->spb;
	sec = sector % s->spb;
	bm  = s->bitmap[blk];
	
	ASSERT(bm && bm->map);

	for (ret = 0; sec < s->spb && ret < nr_secs; sec++, ret++)
		if (test_bit(sec, (void *)bm->map) != value)
			break;

	return ret;
}

static int
allocate_new_block(struct vhd_state *s, uint32_t blk)
{
	u32 buf[128];
	u64 offset = s->bat[blk];
	int new_ftr_pos = s->next_db + s->spb + 1, ret;

	DPRINTF("allocating new block: %u\n", blk);

	/* push footer forward */
	ret = lseek(s->fd, new_ftr_pos << SECTOR_SHIFT, SEEK_SET);
	if (ret == (off_t)-1) {
		DPRINTF("ERROR: seeking footer extension.\n");
		return -errno;
	}
	ret = vhd_write_hd_ftr(s->fd, &s->ftr);
	if (ret != sizeof(struct hd_ftr)) {
		DPRINTF("ERROR: writing footer. %d\n", errno);
		return -errno;
	}

	/* write empty bitmap */
	memset(buf, 0, 512);
	ret = lseek(s->fd, s->next_db << SECTOR_SHIFT, SEEK_SET);
	if (ret == (off_t)-1) {
		DPRINTF("ERROR: seeking bitmap extension.\n");
		return -errno;
	}
	ret = write(s->fd, buf, 512);
	if (ret != 512) {
		DPRINTF("ERROR: writing bitmap. %d,%d\n", ret, errno);
		return -errno;
	}

	/* update bat.  do this as a sector-sized write. */
	s->bat[blk] = s->next_db;
	memcpy(buf, &s->bat[blk - (blk % 128)], 512);
	for (i = 0; i < 128; i++)
		BE32_OUT(&buf[i]);
	ret = lseek(s->fd, 
		    s->hdr.table_offset + (blk - (blk % 128)) * 4, SEEK_SET);
	if (ret == (off_t)-1) {
		DPRINTF("ERROR: seeking bat update.\n");
		return -errno;
	}
	ret = write(s->fd, buf, 512);
	if (ret != 512) {
		DPRINTF("ERROR: updating bat. %d\n", errno);
		return -errno;
	}
	
	s->next_db = new_ftr_pos;

	return 0;
}

static int 
schedule_data_read(struct vhd_state *s, uint64_t sector,
		   int nr_secs, char *buf, u8_t flags,
		   td_callback_t cb, int id, void *private)
{
	u64 offset;
	u32 blk, sec;
	struct iocb *io;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	blk    = sector / s->spb;
	sec    = sector % s->spb;
	bm     = s->bitmap[blk];
	offset = s->bat[blk];

	ASSERT(offset != DD_BLK_UNUSED);
	ASSERT(bm && bm->map);
	ASSERT(read_bitmap_cache_span(s, sector, nr_secs, 1) == nr_secs);

	offset <<= SECTOR_SHIFT;
	offset  += s->bm_size + (sec * VHD_SECTOR_SIZE);

	req = alloc_vhd_request(s);
	if (!req) 
		goto no_mem;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->flags   = flags;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = VHD_OP_DATA_READ;

	io           = &req->iocb;
	io->data     = (void *)req;
	io_prep_read(io, s->fd, req->buf, 
		     req->nr_secs * VHD_SECTOR_SIZE, offset);

	s->iocb_queue[s->iocb_queued++] = io;

	DPRINTF("data read scheduled: lsec: %llu, blk: %u, sec: %u, "
		"nr_secs: %u, offset: %llu, flags: %u\n", 
		req->lsec, blk,	sec, req->nr_secs, offset, req->flags);
	
	return 0;

 no_mem:
	DPRINTF("ERROR: %s: -ENOMEM\n", __func__);
	return -ENOMEM;
}

static int
schedule_data_write(struct vhd_state *s, uint64_t sector,
		    int nr_secs, char *buf, u8_t flags,
		    td_callback_t cb, int id, void *private)
{
	u64 offset;
	u32 blk, sec;
	struct iocb *io;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	blk    = sector / s->spb;
	sec    = sector % s->spb;

	req = alloc_vhd_request(s);
	if (!req)
		goto no_mem;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->flags   = flags;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = VHD_OP_DATA_WRITE;

	if (flags & VHD_FLAG_UPDATE_BAT) {
		ASSERT(s->bat[blk] == DD_BLK_UNUSED && !s->bitmap[blk]);

		/* allocate new block on disk.  done synchronously for now. */
		err = allocate_new_block(s, blk);
		if (err)
			return -EIO;

		/* install empty bitmap in cache */
		bm = calloc(1, sizeof(struct vhd_bitmap));
		if (!bm)
			goto no_mem;
		if (posix_memalign((void **)&bm->map, 512, s->bm_size)) 
			goto no_mem;
		if (posix_memalign((void **)&bm->shadow, 512, s->bm_size))
			goto no_mem;

		memset(bm->map, 0, s->bm_size);
		memset(bm->shadow, 0, s->bm_size);

		s->bitmap[blk] = bm;
	}

	bm     = s->bitmap[blk];
	offset = s->bat[blk];

	ASSERT(offset != DD_BLK_UNUSED);
	ASSERT(bm && bm->map);

	offset <<= SECTOR_SHIFT;
	offset  += s->bm_size + (sec * VHD_SECTOR_SIZE);

	if (flags & VHD_FLAG_UPDATE_BITMAP) {
		if (!bm->write_pending) {
			/* include this bitmap modification in the current
			 * write transaction. */
			++bm->writes_started;
		} else {
			/* bitmap write transaction already started. 
			 * this bitmap modification will be handled after
			 * the completion of the current transaction. */
			req->flags |= VHD_FLAG_ENQUEUE;
		}
	}

	io = &req->iocb;
	io->data = (void *)req;
	io_prep_write(io, s->fd, req->buf,
		      req->nr_secs * VHD_SECTOR_SIZE, offset);

	s->iocb_queue[s->iocb_queued++] = io;
	
	DPRINTF("data write scheduled: lsec: %llu, blk: %u, sec: %u, "
		"nr_secs: %u, offset: %llu, flags: %u\n", 
		req->lsec, blk, sec, req->nr_secs, offset, req->flags);

	return 0;

 no_mem:
	if (bm) {
		if (bm->map)
			free(bm->map);
		free(bm);
	}
	DPRINTF("ERROR: %s: -ENOMEM\n", __func__);
	return -ENOMEM;
}

static int 
schedule_bitmap_read(struct vhd_state *s, uint32_t blk)
{
	char *map;
	u64 offset;
	struct iocb *io;
	struct vhd_bitmap *bm;
	struct vhd_request *req = NULL;

	offset = s->bat[blk];

	ASSERT(offset != DD_BLK_UNUSED);
	ASSERT(!s->bitmap[blk]);

	offset <<= SECTOR_SHIFT;

	req = __alloc_vhd_request(s, VHD_REQ_METADATA);

	bm = calloc(1, sizeof(struct vhd_bitmap));
	if (!bm)
		goto no_mem;

	if (posix_memalign((void **)&map, 512, s->bm_size))
		goto no_mem;

	if (posix_memalign((void **)&bm->shadow, 512, s->bm_size))
		goto no_mem;
	
	req->lsec    = blk * s->spb;
	req->nr_secs = s->bm_size / VHD_SECTOR_SIZE;
	req->buf     = map;
	req->op      = VHD_OP_BITMAP_READ;

	io           = &req->iocb;
	io->data     = (void *)req;
	io_prep_read(io, s->fd, req->buf, s->bm_size, offset);

	s->iocb_queue[s->iocb_queued++] = io;
	s->bitmap[blk] = bm;

	DPRINTF("bitmap read scheduled: lsec: %llu, blk: %u, "
		"nr_secs: %u, offset: %llu\n", 
		req->lsec, blk, req->nr_secs, offset);

	return 0;

 no_mem:
	if (req)
		__free_vhd_request(s, req, VHD_REQ_METADATA);
	if (bm)
		free(bm);
	if (map)
		free(map);
	DPRINTF("ERROR: %s: -ENOMEM\n", __func__);
	return -ENOMEM;
}

static int
schedule_bitmap_write(struct vhd_state *s, 
		      uint32_t blk, struct vhd_request *wait_list)
{
	u64 offset;
	struct iocb *io;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	bm     = s->bitmap[blk];
	offset = s->bat[blk];

	ASSERT(offset != DD_BLK_UNUSED);
	ASSERT(bm && bm->map);
	ASSERT(!bm->write_pending);
	
	offset <<= SECTOR_SHIFT;

	req = __alloc_vhd_request(s, VHD_REQ_METADATA);

	req->lsec    = blk * s->spb;
	req->nr_secs = s->bm_size / VHD_SECTOR_SIZE;
	req->buf     = bm->shadow;
	req->op      = VHD_OP_BITMAP_WRITE;
	req->next    = wait_list;

	io           = &req->iocb;
	io->data     = (void *)req;
	io_prep_write(io, s->fd, req->buf, s->bm_size, offset);

	s->iocb_queue[s->iocb_queued++] = io;
	bm->write_pending = 1;

	DPRINTF("bitmap write scheduled: lsec: %llu, blk: %u, "
		"nr_secs: %u, offset: %llu, waiters: %p\n", 
		req->lsec, blk,	req->nr_secs, offset, req->next);
	
	return 0;
}

/* 
 * queued requests will be submitted once the bitmap
 * describing them is read and the requests are validated. 
 */
static int
__vhd_queue_request(struct vhd_state *s, u8_t op, 
		    uint64_t sector, int nr_secs, char *buf,
		    td_callback_t cb, int id, void *private)
{
	u32 blk;
	struct vhd_bitmap  *bm;
	struct vhd_request *req;

	blk = sector / s->spb;
	bm  = s->bitmap[blk];

	ASSERT(bm && !bm->map);

	req = alloc_vhd_request(s);
	if (!req) 
		goto no_mem;

	req->lsec    = sector;
	req->nr_secs = nr_secs;
	req->buf     = buf;
	req->cb      = cb;
	req->id      = id;
	req->private = private;
	req->op      = op;

	add_to_tail(bm->waiting, req);

	DPRINTF("data request queued: lsec: %llu, blk: %u, sec: %u, "
		"nr_secs: %u, offset: %llu\n", 
		req->lsec, blk,	sec, req->nr_secs, offset);
	
	return 0;

 no_mem:
	DPRINTF("ERROR: %s: -ENOMEM\n", __func__);
	return -ENOMEM;
}

int
vhd_queue_read(struct td_state *tds, uint64_t sector, 
	       int nb_sectors, char *buf, td_callback_t cb, 
	       int id, void *private) 
{
	uint64_t sec, end;
	struct vhd_state *s = (struct vhd_state *)tds->private;

	DPRINTF("%s: sector: %llu, nb_sectors: %d\n",
		__func__, sector, nb_sectors);

	sec = sector;
	end = sector + nb_sectors;

	while (sec < end) {
		int n = 1, err = 0;

		switch (read_bitmap_cache(s, sec)) {
		case -EINVAL:
			cb(tds, -EINVAL, sec, 1, id, private);
			return -EINVAL;
			
		case VHD_BM_BAT_CLEAR:
			n = s->spb - (sec % s->spb);
			cb(tds, SEC_ABSENT, sec, n, id, private);
			break;

		case VHD_BM_BIT_CLEAR:
			n = read_bitmap_cache_span(s, sec, end - sec, 0);
			cb(tds, SEC_ABSENT, sec, n, id, private);
			break;

		case VHD_BM_BIT_SET:
			n   = read_bitmap_cache_span(s, sec, end - sec, 1);
			err = schedule_data_read(s, sec, n, buf, 0,
						 cb, id, private);
			if (err)
				cb(tds, err, sec, n, id, private);
			break;

		case VHD_BM_BITMAP_NOT_CACHED:
			n   = s->spb - (sec % s->spb);
			err = schedule_bitmap_read(s, sec / s->spb);
			if (err) {
				cb(tds, err, sec, n, id, private);
				break;
			}

			err = __vhd_queue_request(s, VHD_OP_DATA_READ, sec,
						  n, buf, cb, id, private);
			if (err)
				cb(tds, err, sec, n, id, private);

			break;

		case VHD_BM_BITMAP_READ_PENDING:
			n   = s->spb - (sec % s->spb);
			err = __vhd_queue_request(s, VHD_OP_DATA_READ, sec,
						  n, buf, cb, id, private);
			if (err)
				return cb(tds, err, sec, n, id, private);
			break;

		default:
			ASSERT(0);
			break;
		}

		sec += n;
		buf += VHD_SECTOR_SIZE * n;
	}

	return 0;
}

int
vhd_queue_write(struct td_state *tds, uint64_t sector, 
		int nb_sectors, char *buf, td_callback_t cb, 
		int id, void *private) 
{
	uint64_t sec, end;
	struct vhd_state *s = (struct vhd_state *)tds->private;

	DPRINTF("%s: sector: %llu, nb_sectors: %d\n",
		__func__, sector, nb_sectors);

	sec = sector;
	end = sector + nb_sectors;

	while (sec < end) {
		int n = 1, err = 0;

		switch (read_bitmap_cache(s, sec)) {
		case -EINVAL:
			cb(tds, -EINVAL, sec, 1, id, private);
			return -EINVAL;
			
		case VHD_BM_BAT_CLEAR:
			u8_t flags = (VHD_FLAG_UPDATE_BAT | 
				      VHD_FLAG_UPDATE_BITMAP);
			n   = s->spb - (sec % s->spb);
			err = schedule_data_write(s, sec, n, buf, 
						  flags, cb, id, private);
			if (err)
				cb(tds, err, sec, n, id, private);
			break;

		case VHD_BM_BIT_CLEAR:
			u8_t flags = VHD_FLAG_UPDATE_BITMAP;
			n   = read_bitmap_cache_span(s, sec, end - sec, 0);
			err = schedule_data_write(s, sec, n, buf, 
						  flags, cb, id, private);
			if (err)
				cb(tds, err, sec, n, id, private);
			break;

		case VHD_BM_BIT_SET:
			n   = read_bitmap_cache_span(s, sec, end - sec, 1);
			err = schedule_data_write(s, sec, n, buf, 0,
						  cb, id, private);
			if (err)
				cb(tds, err, sec, n, id, private);
			break;

		case VHD_BM_BITMAP_NOT_CACHED:
			n   = s->spb - (sec % s->spb);
			err = schedule_bitmap_read(s, sec / s->spb);
			if (err) {
				cb(tds, err, sec, n, id, private);
				break;
			}

			err = __vhd_queue_request(s, VHD_OP_DATA_WRITE, sec,
						  n, buf, cb, id, private);
			if (err)
				cb(tds, err, sec, n, id, private);

			break;

		case VHD_BM_BITMAP_READ_PENDING:
			n   = s->spb - (sec % s->spb);
			err = __vhd_queue_request(s, VHD_OP_DATA_WRITE, sec,
						  n, buf, cb, id, private);
			if (err)
				cb(tds, err, sec, n, id, private);
			break;

		default:
			ASSERT(0);
			break;
		}

		sec += n;
		buf += VHD_SECTOR_SIZE * n;
	}

	return 0;
}

int vhdsync_queue_read(struct td_state *s, uint64_t sector,
			       int nb_sectors, char *buf, td_callback_t cb,
			       int id, void *private)
{
	struct vhdsync_state *prv = (struct vhdsync_state *)s->private;
	int      size    = nb_sectors * s->sector_size;
	uint64_t offset  = sector * (uint64_t)s->sector_size;
	int i = 0, ret = 0;
        uint64_t off = 0;

        DPRINTF("vhdsync_queue_read (%llx, %d)\n", sector, nb_sectors);

        for ( i = 0; i < nb_sectors; i++ ) {
                ret = get_sector_offset(prv, sector + i, &off, 0);
//                DPRINTF("VHD READ: sect %llx, ret: %d offset: %llx\n",
//                        sector+(i*512), ret, off);
                if (ret == 1) {
                        /* TODO: Coalesce reads to contiguous sectors. */
                        ret = lseek(prv->fd, off, SEEK_SET);
                        if (ret == (off_t)-1) {
                                DPRINTF("ERROR: seeking data block..\n");
                                ret = -1;
                                goto done;
                        }
                        ret = read(prv->fd, buf + (i*512), 512);
                        if (ret !=  512) {
                                DPRINTF("ERROR: reading data block.\n");
                                goto done;
                        }
                } else if (ret == 0) {
                        /* TODO: What about CoW case!? */
                        memset(buf + (i*512), 0, 512);
                } else {
                        goto done;
                }
        }

 done:
        cb(s, (ret < 0) ? ret : 0, id, private);

        return 1;
}


int vhdsync_queue_write(struct td_state *s, uint64_t sector,
			       int nb_sectors, char *buf, td_callback_t cb,
			       int id, void *private)
{
	struct vhdsync_state *prv = (struct vhdsync_state *)s->private;
	int      size    = nb_sectors * s->sector_size;
	uint64_t offset  = sector * (uint64_t)s->sector_size;
	int i = 0, ret = 0;
        uint64_t off = 0;

        DPRINTF("vhdsync_queue_write (%llx, %d)\n", sector, nb_sectors);
        // temporarily disable writes.
        /*cb(s, 0, id, private);
          return 1;*/
        
        for ( i = 0; i < nb_sectors; i++ ) {
                ret = get_sector_offset(prv, sector + i, &off, 1);
                DPRINTF("VHD WRITE: sect %llx, ret: %d offset: %llx\n",
                        sector+(i*512), ret, off);
                if (ret == 1) {
                        /* TODO: Coalesce writess to contiguous sectors. */
                        ret = lseek(prv->fd, off, SEEK_SET);
                        if (ret == (off_t)-1) {
                                DPRINTF("ERROR: seeking data block..\n");
                                ret = -1;
                                goto done;
                        }
                        ret = write(prv->fd, buf + (i*512), 512);
                        if (ret !=  512) {
                                DPRINTF("ERROR: reading data block.\n");
                                goto done;
                        }
                } else if (ret == 0) {
                        /* TODO: What about CoW case!? */
                        memset(buf + (i*512), 0, 512);
                } else {
                        goto done;
                }
        }

 done:
        cb(s, (ret < 0) ? ret : 0, id, private);

        return 1;
}
		
int
vhd_submit(struct td_state *tds)
{
	int ret;
	struct vhd_state *s = (struct vhd_state *)tds->private;

        DPRINTF("%s: submitting %d\n", s->iocb_queued);
	ret = io_submit(s->aio_ctx, s->iocb_queued, s->iocb_queue);
	
	/* FIXME: handle error case.  
	 * (remember remove requests from waiting lists if needed.) */

	s->iocb_queued = 0;

	return ret;	
}

int *
vhd_get_fd(struct td_state *s)
{
	struct vhdsync_state *prv = (struct vhdsync_state *)s->private;
	
	int *fds, i;
        DPRINTF("vhdsync_get_fd\n");

	fds = malloc(sizeof(int) * MAX_IOFD);
	/*initialise the FD array*/
	for(i = 0; i < MAX_IOFD; i++) 
		fds[i] = 0;

	fds[0] = prv->poll_pipe[0];
	return fds;
}

int 
vhd_close(struct td_state *s)
{
	struct vhdsync_state *prv = (struct vhdsync_state *)s->private;
	
        DPRINTF("vhdsync_close\n");

        free(prv->bat);

	close(prv->fd);
	close(prv->poll_pipe[0]);
	close(prv->poll_pipe[1]);
	
	return 0;
}

static int
finish_data_read(struct td_state *tds, struct vhd_request *req, int err)
{
	struct vhd_state *s = (struct vhd_state *)tds->private;
	int rsp = req->cb(tds, err, req->lsec, req->nr_secs,
			  req->id, req->private);
	free_vhd_request(s, req);
	return rsp;
}

static int
finish_data_write(struct td_state *tds, struct vhd_request *req, int err)
{
	int rsp = 0;
	u32 blk, sec;
	struct vhd_request *r;
	struct vhd_bitmap  *bm;
	struct vhd_state   *s = (struct vhd_state *)tds->private;

	blk = req->lsec / s->spb;
	sec = req->lsec % s->spb;
	bm  = s->bitmap[blk];

	ASSERT(bm && bm->map);

	if (req->flags & VHD_FLAG_UPDATE_BITMAP) {
		if (req->flags & VHD_FLAG_ENQUEUE) {
			/* if this request is not included in the current 
			 * bitmap write transaction and it failed, do not
			 * worry about updating the bitmap. */
			if (err)
				goto out;

			if (bm->write_pending) {
				/* this request is not part of current bitmap
				 * write transaction.  its bitmap modifications
				 * will be included in the next transaction. */
				add_to_tail(bm->queued, req);
				return 0;
			} else {
				/* previous bitmap transaction ended while this
				 * request was being satisfied.  add this 
				 * request to current bitmap transaction. */
				++bm->writes_started;
			}
		}

		/* this data write is part of the current bitmap write
		 * transaction.  update the im-memory bitmap and start
		 * disk writeback if no other data writes in this
		 * transaction are pending. */
		if (!err) {
			int i;
			for (i = 0; i < req->nr_secs; i++)
				set_bit(sec + i, (void *)bm->shadow);
			add_to_tail(bm->waiting_writes, req);
		}
		
		/* NB: the last request of a bitmap write transaction must
		 * begin bitmap writeback, even the request itself failed. */
		++bm->writes_finished;
		if (bm->writes_finished == bm->writes_started) {
			r = bm->waiting_writes;
			bm->waiting_writes  = NULL;
			bm->writes_started  = 0;
			bm->writes_finished = 0;
			schedule_bitmap_write(s, blk, r);
		}
	}

 out:
	/* the completion of 
	 * failed writes and writes that did not modify the bitmap
	 * can be signalled immediately */
	if (err || !(req->flags & VHD_FLAG_UPDATE_BITMAP)) {
		rsp += req->cb(tds, err, req->lsec, 
			       req->nr_secs, req->id, req->private);
		free_vhd_request(s, req);
	}

	return rsp;
}

static int
finish_bitmap_read(struct td_state *tds, struct vhd_request *req, int err)
{
	int rsp = 0;
	u32 blk, sec;
	struct vhd_bitmap  *bm;
	struct vhd_request *r, *next;
	struct vhd_state   *s = (struct vhd_state *)tds->private;

	blk = req->lsec / s->spb;
	sec = req->lsec % s->spb;
	bm  = s->bitmap[blk];

	ASSERT(bm && !bm->map);
	r = bm->waiting;
	bm->waiting = NULL;

	if (!err) {
		/* success.  install map. */
		bm->map = req->buf;
		memcpy(bm->shadow, bm->map, s->bm_size);

		/* submit any waiting requests */
		while (r) {
			struct vhd_request tmp;

			tmp  = *r;
			next =  r->next;
			free_vhd_request(s, r);

			ASSERT(tmp.op == VHD_OP_DATA_READ || 
			       tmp.op == VHD_OP_DATA_WRITE);

			if (tmp.op == VHD_OP_DATA_READ)
				vhd_queue_read(tds, tmp.lsec, tmp.nr_secs,
					       tmp.buf, tmp.cb, tmp.id,
					       tmp.private);
			else if (tmp.op == VHD_OP_DATA_WRITE)
				vhd_queue_write(tds, tmp.lsec, tmp.nr_secs,
						tmp.buf, tmp.cb, tmp.id,
						tmp.private);

			r = next;
		}
	} else {
		/* error.  fail any waiting requests. */
		while (r) {
			rsp += r->cb(tds, err, r->lsec, 
				     r->nr_secs, r->id, r->private);

			next = r->next;
			free_vhd_request(s, r);
			r = next;
		}

		/* bitmap is not cached */
		free(bm->shadow);
		free(bm);
		free(req->map);
		s->bitmap[blk] = NULL;
	}

	__free_vhd_request(s, req, VHD_REQ_METADATA);

	return rsp;
}

static int
finish_bitmap_write(struct td_state *tds, struct vhd_request *req, int err)
{
	u32 blk;
	int rsp = 0;
	struct vhd_bitmap  *bm;
	struct vhd_request *r, *next, *bm_list = NULL;
	struct vhd_state   *s = (struct vhd_state *)tds->private;

	blk = req->lsec / s->spb;
	bm  = s->bitmap[blk];

	ASSERT(bm && bm->map);
	ASSERT(bm->write_pending);
	ASSERT(!bm->writes_started &&     /* no new requests should be added */
	       !bm->writes_finished);     /* while a transaction is pending. */

	bm->write_pending = 0;

	if (!err) {
		/* complete atomic write */
		memcpy(bm->map, bm->shadow, s->bm_size);
	} else {
		/* undo changes to shadow */
		memcpy(bm->shadow, bm->map, s->bm_size);
	}

	/* signal completion on any waiting write requests */
	r = req->next;
	while (r) {
		rsp += r->cb(tds, err, r->lsec, r->nr_secs, r->id, r->private);
		next = r->next;
		free_vhd_request(s, r);
		r = next;
	}

	__free_vhd_request(s, req, VHD_REQ_METADATA);

	/* start new transaction if more requests are queued */
	r = bm->queued;
	while (r) {
		int i;
		u32 sec = r->lsec % s->spb;
		
		ASSERT((r->lsec / s->spb) == blk);

		for (i = 0; i < r->nr_secs; i++)
			set_bit(sec + i, (void *)bm->shadow);

		add_to_tail(bm_list, r);
		r = r->next;
	}
	bm->queued = NULL;

	if (bm_list) 
		schedule_bitmap_write(s, blk, bm_list);

	return rsp;
}

int
vhd_do_callbacks(struct td_state *tds, int sid)
{
	struct io_event *ep;
	int ret, i, nr_iocbs, rsp = 0;
	struct vhd_state *s = (struct vhd_state *)tds->private;

	if (sid > MAX_IOFD)
		return 1;

	nr_iocbs = s->iocb_queued;

	/* non-blocking test for completed io */
	ret = io_getevents(s->aio_ctx, 0, 
			   s->total_vreqs, s->aio_events, NULL);

	for (ep = s->aio_events, i = ret; i-- > 0; ep++) {
		int err;
		struct iocb *io = ep->obj;
		struct vhd_request *req = (struct vhd_request *)io->data;

		err = (ep->res == io->u.c.nbytes) ? 0 : -EIO;

		switch (req->op) {
		case VHD_OP_DATA_READ:
			rsp += finish_data_read(tds, req, err);
			break;

		case VHD_OP_DATA_WRITE:
			rsp += finish_data_write(tds, req, err);
			break;

		case VHD_OP_BITMAP_READ:
			rsp += finish_bitmap_read(tds, req, err);
			break;

		case VHD_OP_BITMAP_WRITE:
			rsp += finish_bitmap_write(tds, req, err);
			break;
			
		default:
			ASSERT(0);
			break;
		}
	}

	if (s->iocb_queued != nr_iocbs) {
		/* additional requests were queued.  submit them. */
		vhd_submit(tds);
	}

	return rsp;
}

struct tap_disk tapdisk_vhd = {
	.disk_type          = "tapdisk_vhd",
	.private_data_size  = sizeof(struct vhd_state),
	.td_open            = vhd_open,
	.td_queue_read      = vhd_queue_read,
	.td_queue_write     = vhd_queue_write,
	.td_submit          = vhd_submit,
	.td_get_fd          = vhd_get_fd,
	.td_close           = vhd_close,
	.td_do_callbacks    = vhd_do_callbacks
};
