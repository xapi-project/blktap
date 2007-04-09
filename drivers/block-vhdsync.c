/* block-vhd-sync.c
 *
 * synchronous vhd implementation.
 *
 * (c) 2006 Andrew Warfield
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
//#include <asm/bitops.h>
#include "tapdisk.h"
#include "vhd.h"
#include "bswap.h"

struct vhdsync_state {

	int fd;
	int poll_pipe[2]; /* dummy fd for polling on */

        /* VHD stuff */
        struct hd_ftr  ftr;
        struct dd_hdr  hdr;
        u32            spb; /* sectors per block */
        u32           *bat;
        u32            next_db; /* pointer to the next (unalloced) datablock */
};

/* Helpers: */
#define BE32_IN(foo) (*(foo)) = be32_to_cpu(*(foo))
#define BE64_IN(foo) (*(foo)) = be64_to_cpu(*(foo))
#define BE32_OUT(foo) (*(foo)) = cpu_to_be32(*(foo))
#define BE64_OUT(foo) (*(foo)) = cpu_to_be64(*(foo))

static inline int test_bit ( int nr, volatile void * addr)
{
    return (((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] >>
            (nr % (sizeof(unsigned long)*8))) & 1;
}

static inline void clear_bit ( int nr, volatile void * addr)
{
    ((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] &=
        ~(1 << (nr % (sizeof(unsigned long)*8) ) );
}

static inline void set_bit ( int nr, volatile void * addr)
{
    ((unsigned long*)addr)[nr/(sizeof(unsigned long)*8)] |=
        (1 << (nr % (sizeof(unsigned long)*8) ) );
}

/* Debug print functions: */


/* Stringify the VHD timestamp for printing.                              */
/* As with ctime_r, target must be >=26 bytes.                            */
/* TODO: Verify this is correct.                                          */
static size_t vhd_time_to_s(u32 timestamp, char *target)
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

static u32 f_checksum( struct hd_ftr *f )
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

static void debug_print_footer( struct hd_ftr *f )
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
    DPRINTF("File format version : Major: %d, Minor: %d\n", ff_maj, ff_min);

    DPRINTF("Data offset         : %lld\n", f->data_offset);
    
    vhd_time_to_s(f->timestamp, time_str);
    DPRINTF("Timestamp           : %s\n", time_str);
    
    memcpy(creator, f->crtr_app, 4);
    creator[4] = '\0';
    DPRINTF("Creator Application : '%s'\n", creator);

    cr_maj = f->crtr_ver >> 16;
    cr_min = f->crtr_ver & 0xffff;
    DPRINTF("Creator version     : Major: %d, Minor: %d\n", cr_maj, cr_min);

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
           f->type <= HD_TYPE_MAX ? HD_TYPE_STR[f->type] : "Unknown type!\n");

    cksm = f_checksum(f);
    DPRINTF("Checksum            : 0x%x|0x%x (%s)\n", f->checksum, cksm,
           f->checksum == cksm ? "Good!" : "Bad!" );

    uuid_unparse(f->uuid, uuid);
    DPRINTF("UUID                : %s\n", uuid);

    DPRINTF("Saved state         : %s\n", f->saved == 0 ? "No" : "Yes" );
}

static u32 h_checksum( struct dd_hdr *h )
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

static void debug_print_header( struct dd_hdr *h )
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

static int vhd_read_hd_ftr(int fd, struct hd_ftr *ftr)
{
    off_t vhd_end;

    memset(ftr, 0, sizeof(struct hd_ftr));

    /* Not sure if it's generally a good idea to use SEEK_END with -ve     */
    /* offsets, so do one seek to find the end of the file, and then use   */
    /* SEEK_SETs when searching for the footer.                            */
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
    
    /* According to the spec, pre-Virtual PC 2004 VHDs used a 511B footer. */
    /* Try that...                                                         */
    if ( lseek64(fd, vhd_end - 511, SEEK_SET) == -1 ) {
        return -1;
    }
    if ( read(fd, ftr, 511 != 511) ) {
        return -1;
    }
    if ( memcmp(ftr->cookie, HD_COOKIE,  8) == 0 ) goto found_footer;
    
    /* Last try -- look for the copy of the footer at the start of image.  */
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
static int vhd_write_hd_ftr(int fd, struct hd_ftr *in_use_ftr)
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

static int vhd_read_dd_hdr(int fd, struct dd_hdr *hdr, u64 location)
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

static int vhd_read_bat(int fd, struct vhdsync_state *prv)
{
    int i, count = 0;
    u32 entries = prv->hdr.max_bat_size;
    u64 location = prv->hdr.table_offset;
    
    DPRINTF("Reading BAT at %lld, %d entries.\n", location, entries);    

    if ( lseek64(fd, location, SEEK_SET) == -1 ) {
       return -1;
    }
    if ( read(fd, prv->bat, sizeof(u32) * entries) 
         != (sizeof(u32) * entries) ) {
       return -1;
    }
    
    prv->next_db = location >> SECTOR_SHIFT; /* BAT is sector aligned. */
    prv->next_db += ((sizeof(u32)*entries) + (512-1)) >> SECTOR_SHIFT;
    DPRINTF("FirstDB: %d\n", prv->next_db);

    for (i=0; i<entries; i++) {
            BE32_IN(&prv->bat[i]);
            if ((prv->bat[i] != 0xFFFFFFFF) && (prv->bat[i] > prv->next_db)) {
                    prv->next_db = prv->bat[i] + prv->spb + 1 /* bitmap */; 
                    DPRINTF("i: %d, prv->bat[i]: %u, spb: %d, next: %u\n", 
                            i, prv->bat[i], prv->spb,  prv->next_db);
            }

            if (prv->bat[i] != 0xFFFFFFFF) count++;
    }
    
    DPRINTF("NextDB: %d\n", prv->next_db);
    /* Test code -- the vhd file i have seems to have an extra data block. */
#if 0
    {
            char bitmap[512];
            lseek(fd, prv->next_db << SECTOR_SHIFT, SEEK_SET);
            read(fd, bitmap, 512);
            DPRINTF("SCANNING EXTRA BITMAP:\n");
            for (i=0; i<512; i++) if (bitmap[i] != 0) 
                    DPRINTF("bitmap[%d] is not zero\n", i);
    }
#endif
    DPRINTF("Read BAT.  This vhd has %d full and %d unfilled data blocks.\n", 
            count, entries - count);
    
    return 0;
}

static void inline init_fds(struct disk_driver *dd)
{
	int i;
	struct vhdsync_state *prv = (struct vhdsync_state *)dd->private;

	for(i = 0; i < MAX_IOFD; i++)
		dd->io_fd[i] = 0;

	dd->io_fd[0] = prv->poll_pipe[0];
}

/*Get Image size, secsize*/
int vhdsync_open (struct disk_driver *dd, const char *name, td_flag_t flags)
{
        int fd, ret = 0, o_flags;
	struct td_state      *s   = dd->td_state;
	struct vhdsync_state *prv = (struct vhdsync_state *)dd->private;

        DPRINTF("vhdsync_open: %s\n", name);

        /* set up a pipe so that we can hand back a poll fd that won't fire.*/
	ret = pipe(prv->poll_pipe);
	if (ret != 0)
		return (0 - errno);

        prv->bat = NULL;

        /* Read the disk footer. */
	o_flags = O_LARGEFILE | ((flags == TD_RDONLY) ? O_RDONLY : O_RDWR);
        fd = open(name, o_flags);
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
	init_fds(dd);
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
static int get_sector_offset(struct vhdsync_state *prv, uint64_t sector, 
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
        if ((db_start = (u64)prv->bat[blk]) == 0xFFFFFFFF) {
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

int vhdsync_queue_read(struct disk_driver *dd, uint64_t sector,
		       int nb_sectors, char *buf, td_callback_t cb,
		       int id, void *private)
{
	struct td_state      *s   = dd->td_state;
	struct vhdsync_state *prv = (struct vhdsync_state *)dd->private;
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
        return cb(dd, (ret < 0) ? ret : 0, sector, nb_sectors, id, private);
}


int vhdsync_queue_write(struct disk_driver *dd, uint64_t sector,
			int nb_sectors, char *buf, td_callback_t cb,
			int id, void *private)
{
	struct td_state      *s   = dd->td_state;
	struct vhdsync_state *prv = (struct vhdsync_state *)dd->private;
	int      size    = nb_sectors * s->sector_size;
	uint64_t offset  = sector * (uint64_t)s->sector_size;
	int i = 0, ret = 0;
        uint64_t off = 0;

        DPRINTF("vhdsync_queue_write (%llx, %d)\n", sector, nb_sectors);
        
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
        return cb(dd, (ret < 0) ? ret : 0, sector, nb_sectors, id, private);
}
		
int vhdsync_submit(struct disk_driver *dd)
{
        //DPRINTF("vhdsync_submit\n");
	return 0;	
}

int vhdsync_close(struct disk_driver *dd)
{
	struct vhdsync_state *prv = (struct vhdsync_state *)dd->private;
	
        DPRINTF("vhdsync_close\n");

        free(prv->bat);

	close(prv->fd);
	close(prv->poll_pipe[0]);
	close(prv->poll_pipe[1]);
	
	return 0;
}

int vhdsync_do_callbacks(struct disk_driver *dd, int sid)
{
	/* always ask for a kick */
	return 1;
}

int vhdsync_get_parent_id(struct disk_driver *dd, struct disk_id *id)
{
	return TD_NO_PARENT;
}

int vhdsync_validate_parent(struct disk_driver *dd, 
			    struct disk_driver *parent, td_flag_t flags)
{
	return -EINVAL;
}

struct tap_disk tapdisk_vhdsync = {
	.disk_type              = "tapdisk_vhdsync",
	.private_data_size      = sizeof(struct vhdsync_state),
	.td_open                = vhdsync_open,
	.td_queue_read          = vhdsync_queue_read,
	.td_queue_write         = vhdsync_queue_write,
	.td_submit              = vhdsync_submit,
	.td_close               = vhdsync_close,
	.td_do_callbacks        = vhdsync_do_callbacks,
	.td_get_parent_id       = vhdsync_get_parent_id,
	.td_validate_parent     = vhdsync_validate_parent,
	.td_snapshot            = NULL,
	.td_create              = NULL
};
