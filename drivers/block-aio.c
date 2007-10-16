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
#include <libaio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "tapdisk.h"


#define MAX_AIO_REQS         TAPDISK_DATA_REQUESTS

struct aio_request {
	int                  id;
	uint64_t             lsec;
	int                  secs;
	void                *private;
	struct tiocb         tiocb;
	td_callback_t        cb;
};

struct tdaio_state {
	int                  fd;

	int                  aio_free_count;	
	struct aio_request   aio_requests[MAX_AIO_REQS];
	struct aio_request  *aio_free_list[MAX_AIO_REQS];
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
		s->size =((uint64_t) 16836057);
		s->sector_size = DEFAULT_SECTOR_SIZE;
	}
	s->info = 0;

	return 0;
}

/* Open the disk file and initialize aio state. */
int tdaio_open (struct disk_driver *dd, const char *name, td_flag_t flags)
{
	int i, fd, ret = 0, o_flags;
	struct td_state    *s   = dd->td_state;
	struct tdaio_state *prv = (struct tdaio_state *)dd->private;

	DPRINTF("block-aio open('%s')", name);

	memset(prv, 0, sizeof(struct tdaio_state));

	prv->aio_free_count = MAX_AIO_REQS;
	for (i = 0; i < MAX_AIO_REQS; i++)
		prv->aio_free_list[i] = &prv->aio_requests[i];

	/* Open the file */
	o_flags = O_DIRECT | O_LARGEFILE | 
		((flags & TD_RDONLY) ? O_RDONLY : O_RDWR);
        fd = open(name, o_flags);

        if ( (fd == -1) && (errno == EINVAL) ) {

                /* Maybe O_DIRECT isn't supported. */
		o_flags &= ~O_DIRECT;
                fd = open(name, o_flags);
                if (fd != -1) DPRINTF("WARNING: Accessing image without"
                                     "O_DIRECT! (%s)\n", name);

        } else if (fd != -1) DPRINTF("open(%s) with O_DIRECT\n", name);
	
        if (fd == -1) {
		DPRINTF("Unable to open [%s] (%d)!\n", name, 0 - errno);
        	ret = 0 - errno;
        	goto done;
        }

        prv->fd = fd;

	ret = get_image_info(s, fd);

done:
	return ret;	
}

int tdaio_queue_read(struct disk_driver *dd, uint64_t sector,
		     int nb_sectors, char *buf, td_callback_t cb,
		     int id, void *private)
{
	struct   aio_request *aio;
	struct   td_state    *s   = dd->td_state;
	struct   tdaio_state *prv = (struct tdaio_state *)dd->private;
	int      size    = nb_sectors * s->sector_size;
	uint64_t offset  = sector * (uint64_t)s->sector_size;

	if (prv->aio_free_count == 0)
		return -EBUSY;
	aio = prv->aio_free_list[--prv->aio_free_count];

	aio->cb      = cb;
	aio->id      = id;
	aio->lsec    = sector;
	aio->secs    = nb_sectors;
	aio->private = private;

	td_prep_read(&aio->tiocb, dd, prv->fd, buf, size, offset, aio);
	td_queue_tiocb(dd, &aio->tiocb);

	return 0;
}
			
int tdaio_queue_write(struct disk_driver *dd, uint64_t sector,
		      int nb_sectors, char *buf, td_callback_t cb,
		      int id, void *private)
{
	struct   aio_request *aio;
	struct   td_state    *s   = dd->td_state;
	struct   tdaio_state *prv = (struct tdaio_state *)dd->private;
	int      size    = nb_sectors * s->sector_size;
	uint64_t offset  = sector * (uint64_t)s->sector_size;

	if (prv->aio_free_count == 0)
		return -EBUSY;
	aio = prv->aio_free_list[--prv->aio_free_count];

	aio->cb      = cb;
	aio->id      = id;
	aio->lsec    = sector;
	aio->secs    = nb_sectors;
	aio->private = private;

	td_prep_write(&aio->tiocb, dd, prv->fd, buf, size, offset, aio);
	td_queue_tiocb(dd, &aio->tiocb);

	return 0;
}

int tdaio_close(struct disk_driver *dd)
{
	struct tdaio_state *prv = (struct tdaio_state *)dd->private;
	
	close(prv->fd);

	return 0;
}

int tdaio_complete(struct disk_driver *dd, struct tiocb *tiocb, int err)
{
	struct tdaio_state *prv = (struct tdaio_state *)dd->private;
	struct aio_request *aio = tiocb->data;
	struct iocb *io = &tiocb->iocb;

	aio->cb(dd, err, aio->lsec, aio->secs, aio->id, aio->private);
	prv->aio_free_list[prv->aio_free_count++] = aio;

	return 0;
}

int tdaio_get_parent_id(struct disk_driver *dd, struct disk_id *id)
{
	return TD_NO_PARENT;
}

int tdaio_validate_parent(struct disk_driver *dd, 
			  struct disk_driver *parent, td_flag_t flags)
{
	return -EINVAL;
}

struct tap_disk tapdisk_aio = {
	.disk_type          = "tapdisk_aio",
	.private_data_size  = sizeof(struct tdaio_state),
	.private_iocbs      = 0,
	.td_open            = tdaio_open,
	.td_close           = tdaio_close,
	.td_queue_read      = tdaio_queue_read,
	.td_queue_write     = tdaio_queue_write,
	.td_complete        = tdaio_complete,
	.td_get_parent_id   = tdaio_get_parent_id,
	.td_validate_parent = tdaio_validate_parent,
	.td_snapshot        = NULL,
	.td_create          = NULL
};
