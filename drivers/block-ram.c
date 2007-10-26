/* 
 * Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <string.h>
#include "tapdisk.h"

char *img;
long int   disksector_size;
long int   disksize;
long int   diskinfo;
static int connections = 0;

struct tdram_state {
        int fd;
};

/*Get Image size, secsize*/
static int get_image_info(struct td_state *s, int fd)
{
	int ret;
	long size;
	unsigned long total_size;
	struct statvfs statBuf;
	struct stat stat;

	ret = fstat(fd, &stat);
	if (ret != 0) {
		DPRINTF("ERROR: fstat failed, Couldn't stat image");
		return -EINVAL;
	}

	if (S_ISBLK(stat.st_mode)) {
		/*Accessing block device directly*/
		s->size = 0;
		if (ioctl(fd,BLKGETSIZE,&s->size)!=0) {
			DPRINTF("ERR: BLKGETSIZE failed, couldn't stat image");
			return -EINVAL;
		}

		DPRINTF("Image size: \n\tpre sector_shift  [%llu]\n\tpost "
			"sector_shift [%llu]\n",
			(long long unsigned)(s->size << SECTOR_SHIFT),
			(long long unsigned)s->size);

		/*Get the sector size*/
#if defined(BLKSSZGET)
		{
			int arg;
			s->sector_size = DEFAULT_SECTOR_SIZE;
			ioctl(fd, BLKSSZGET, &s->sector_size);
			
			if (s->sector_size != DEFAULT_SECTOR_SIZE)
				DPRINTF("Note: sector size is %ld (not %d)\n",
					s->sector_size, DEFAULT_SECTOR_SIZE);
		}
#else
		s->sector_size = DEFAULT_SECTOR_SIZE;
#endif

	} else {
		/*Local file? try fstat instead*/
		s->size = (stat.st_size >> SECTOR_SHIFT);
		s->sector_size = DEFAULT_SECTOR_SIZE;
		DPRINTF("Image size: \n\tpre sector_shift  [%llu]\n\tpost "
			"sector_shift [%llu]\n",
			(long long unsigned)(s->size << SECTOR_SHIFT),
			(long long unsigned)s->size);
	}

	if (s->size == 0) {		
		s->size =((uint64_t) MAX_RAMDISK_SIZE);
		s->sector_size = DEFAULT_SECTOR_SIZE;
	}
	s->info = 0;

        /*Store variables locally*/
	disksector_size = s->sector_size;
	disksize        = s->size;
	diskinfo        = s->info;
	DPRINTF("Image sector_size: \n\t[%lu]\n",
		s->sector_size);

	return 0;
}

/* Open the disk file and initialize ram state. */
int tdram_open (struct disk_driver *dd, const char *name, td_flag_t flags)
{
	char *p;
	uint64_t size;
	int i, fd, ret = 0, count = 0, o_flags;
	struct td_state    *s     = dd->td_state;
	struct tdram_state *prv   = (struct tdram_state *)dd->private;

	connections++;

	if (connections > 1) {
		s->sector_size = disksector_size;
		s->size        = disksize;
		s->info        = diskinfo; 
		DPRINTF("Image already open, returning parameters:\n");
		DPRINTF("Image size: \n\tpre sector_shift  [%llu]\n\tpost "
			"sector_shift [%llu]\n",
			(long long unsigned)(s->size << SECTOR_SHIFT),
			(long long unsigned)s->size);
		DPRINTF("Image sector_size: \n\t[%lu]\n",
			s->sector_size);

		prv->fd = -1;
		goto done;
	}

	/* Open the file */
	o_flags = O_DIRECT | O_LARGEFILE | 
		((flags == TD_OPEN_RDONLY) ? O_RDONLY : O_RDWR);
        fd = open(name, o_flags);

        if ((fd == -1) && (errno == EINVAL)) {

                /* Maybe O_DIRECT isn't supported. */
		o_flags &= ~O_DIRECT;
                fd = open(name, o_flags);
                if (fd != -1) DPRINTF("WARNING: Accessing image without"
                                     "O_DIRECT! (%s)\n", name);

        } else if (fd != -1) DPRINTF("open(%s) with O_DIRECT\n", name);
	
        if (fd == -1) {
		DPRINTF("Unable to open [%s]!\n",name);
        	ret = 0 - errno;
        	goto done;
        }

        prv->fd = fd;

	ret = get_image_info(s, fd);
	size = MAX_RAMDISK_SIZE;

	if (s->size > size) {
		DPRINTF("Disk exceeds limit, must be less than [%d]MB",
			(MAX_RAMDISK_SIZE<<SECTOR_SHIFT)>>20);
		return -ENOMEM;
	}

	/*Read the image into memory*/
	if (posix_memalign((void **)&img, 
			   DEFAULT_SECTOR_SIZE, s->size << SECTOR_SHIFT)) {
		DPRINTF("Mem malloc failed\n");
		return -errno;
	}
	p = img;
	DPRINTF("Reading %llu bytes.......",(long long unsigned)s->size << SECTOR_SHIFT);

	for (i = 0; i < s->size; i++) {
		ret = read(prv->fd, p, s->sector_size);
		if (ret != s->sector_size) {
			DPRINTF("ret = %d, errno = %d\n", ret, errno);
			ret = 0 - errno;
			break;
		} else {
			count += ret;
			p = img + count;
		}
	}
	DPRINTF("[%d]\n",count);
	if (count != s->size << SECTOR_SHIFT) {
		ret = -1;
	} else {
		ret = 0;
	}

done:
	return ret;
}

 int tdram_queue_read(struct disk_driver *dd, uint64_t sector,
		      int nb_sectors, char *buf, td_callback_t cb,
		      int id, void *private)
{
	struct td_state    *s   = dd->td_state;
	struct tdram_state *prv = (struct tdram_state *)dd->private;
	int      size    = nb_sectors * s->sector_size;
	uint64_t offset  = sector * (uint64_t)s->sector_size;

	memcpy(buf, img + offset, size);

	return cb(dd, 0, sector, nb_sectors, id, private);
}

int tdram_queue_write(struct disk_driver *dd, uint64_t sector,
		      int nb_sectors, char *buf, td_callback_t cb,
		      int id, void *private)
{
	struct td_state    *s   = dd->td_state;
	struct tdram_state *prv = (struct tdram_state *)dd->private;
	int      size    = nb_sectors * s->sector_size;
	uint64_t offset  = sector * (uint64_t)s->sector_size;
	
	/* We assume that write access is controlled
	 * at a higher level for multiple disks */
	memcpy(img + offset, buf, size);

	return cb(dd, 0, sector, nb_sectors, id, private);
}

int tdram_close(struct disk_driver *dd)
{
	struct tdram_state *prv = (struct tdram_state *)dd->private;
	
	connections--;
	
	return 0;
}

int tdram_get_parent_id(struct disk_driver *dd, struct disk_id *id)
{
	return TD_NO_PARENT;
}

int tdram_validate_parent(struct disk_driver *dd, 
			  struct disk_driver *parent, td_flag_t flags)
{
	return -EINVAL;
}

struct tap_disk tapdisk_ram = {
	.disk_type          = "tapdisk_ram",
	.private_data_size  = sizeof(struct tdram_state),
	.private_iocbs      = 0,
	.td_open            = tdram_open,
	.td_close           = tdram_close,
	.td_queue_read      = tdram_queue_read,
	.td_queue_write     = tdram_queue_write,
	.td_complete        = NULL,
	.td_get_parent_id   = tdram_get_parent_id,
	.td_validate_parent = tdram_validate_parent,
	.td_snapshot        = NULL,
	.td_create          = NULL
};
