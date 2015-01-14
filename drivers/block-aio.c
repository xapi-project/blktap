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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "tapdisk.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "block-aio.h"



/*Get Image size, secsize*/
static int tdaio_get_image_info(int fd, td_disk_info_t *info)
{
	int ret;
	unsigned long long bytes;
	struct stat stat;

	ret = fstat(fd, &stat);
	if (ret != 0) {
		DPRINTF("ERROR: fstat failed, Couldn't stat image");
		return -EINVAL;
	}

	if (S_ISBLK(stat.st_mode)) {
		/*Accessing block device directly*/
		info->size = 0;
		if (ioctl(fd,BLKGETSIZE64,&bytes)==0) {
			info->size = bytes >> SECTOR_SHIFT;
		} else if (ioctl(fd,BLKGETSIZE,&info->size)!=0) {
			DPRINTF("ERR: BLKGETSIZE and BLKGETSIZE64 failed, couldn't stat image");
			return -EINVAL;
		}

		DPRINTF("Image size: \n\tpre sector_shift  [%llu]\n\tpost "
			"sector_shift [%llu]\n",
			(long long unsigned)(info->size << SECTOR_SHIFT),
			(long long unsigned)info->size);

		/*Get the sector size*/
#if defined(BLKSSZGET)
		{
			info->sector_size = DEFAULT_SECTOR_SIZE;
			ioctl(fd, BLKSSZGET, &info->sector_size);
			
			if (info->sector_size != DEFAULT_SECTOR_SIZE)
				DPRINTF("Note: sector size is %ld (not %d)\n",
					info->sector_size, DEFAULT_SECTOR_SIZE);
		}
#else
		info->sector_size = DEFAULT_SECTOR_SIZE;
#endif

	} else {
		/*Local file? try fstat instead*/
		info->size = (stat.st_size >> SECTOR_SHIFT);
		info->sector_size = DEFAULT_SECTOR_SIZE;
		DPRINTF("Image size: \n\tpre sector_shift  [%llu]\n\tpost "
			"sector_shift [%llu]\n",
			(long long unsigned)(info->size << SECTOR_SHIFT),
			(long long unsigned)info->size);
	}

	if (info->size == 0) {		
		info->size =((uint64_t) 16836057);
		info->sector_size = DEFAULT_SECTOR_SIZE;
	}
	info->info = 0;

	return 0;
}

/* Open the disk file and initialize aio state. */
int tdaio_open(td_driver_t *driver, const char *name, td_flag_t flags)
{
	int i, fd, ret, o_flags;
	struct tdaio_state *prv;

	ret = 0;
	prv = (struct tdaio_state *)driver->data;

	DPRINTF("block-aio open('%s')", name);

	memset(prv, 0, sizeof(struct tdaio_state));

	prv->aio_free_count = MAX_AIO_REQS;
	for (i = 0; i < MAX_AIO_REQS; i++)
		prv->aio_free_list[i] = &prv->aio_requests[i];

	/* Open the file */
	o_flags = O_DIRECT | O_LARGEFILE | 
		((flags & TD_OPEN_RDONLY) ? O_RDONLY : O_RDWR);
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

	ret = tdaio_get_image_info(fd, &driver->info);
	if (ret) {
		close(fd);
		goto done;
	}

        prv->fd = fd;

done:
	return ret;	
}

void tdaio_complete(void *arg, struct tiocb *tiocb, int err)
{
	struct aio_request *aio = (struct aio_request *)arg;
	struct tdaio_state *prv = aio->state;

	td_complete_request(aio->treq, err);
	prv->aio_free_list[prv->aio_free_count++] = aio;
}

void tdaio_queue_read(td_driver_t *driver, td_request_t treq)
{
	int size;
	uint64_t offset;
	struct aio_request *aio;
	struct tdaio_state *prv;

	prv    = (struct tdaio_state *)driver->data;
	size   = treq.secs * SECTOR_SIZE;
	offset = treq.sec  * (uint64_t)SECTOR_SIZE;

	if (prv->aio_free_count == 0)
		goto fail;

	aio        = prv->aio_free_list[--prv->aio_free_count];
	aio->treq  = treq;
	aio->state = prv;

	td_prep_read(&aio->tiocb, prv->fd, treq.buf,
		     size, offset, tdaio_complete, aio);
	td_queue_tiocb(driver, &aio->tiocb);

	return;

fail:
	td_complete_request(treq, -EBUSY);
}

void tdaio_queue_write(td_driver_t *driver, td_request_t treq)
{
	int size;
	uint64_t offset;
	struct aio_request *aio;
	struct tdaio_state *prv;

	prv     = (struct tdaio_state *)driver->data;
	size    = treq.secs * SECTOR_SIZE;
	offset  = treq.sec  * (uint64_t)SECTOR_SIZE;

	if (prv->aio_free_count == 0)
		goto fail;

	aio        = prv->aio_free_list[--prv->aio_free_count];
	aio->treq  = treq;
	aio->state = prv;

	td_prep_write(&aio->tiocb, prv->fd, treq.buf,
		      size, offset, tdaio_complete, aio);
	td_queue_tiocb(driver, &aio->tiocb);

	return;

fail:
	td_complete_request(treq, -EBUSY);
}

int tdaio_close(td_driver_t *driver)
{
	struct tdaio_state *prv = (struct tdaio_state *)driver->data;
	
	close(prv->fd);

	return 0;
}

int tdaio_get_parent_id(td_driver_t *driver, td_disk_id_t *id)
{
	return TD_NO_PARENT;
}

int tdaio_validate_parent(td_driver_t *driver,
			  td_driver_t *pdriver, td_flag_t flags)
{
	return -EINVAL;
}

void tdaio_stats(td_driver_t *driver, td_stats_t *st)
{
	struct tdaio_state *prv = (struct tdaio_state *)driver->data;
	int n_pending;

	n_pending = MAX_AIO_REQS - prv->aio_free_count;

	tapdisk_stats_field(st, "reqs", "{");
	tapdisk_stats_field(st, "max", "lu", MAX_AIO_REQS);
	tapdisk_stats_field(st, "pending", "d", n_pending);
	tapdisk_stats_leave(st, '}');
}

struct tap_disk tapdisk_aio = {
	.disk_type          = "tapdisk_aio",
	.flags              = 0,
	.private_data_size  = sizeof(struct tdaio_state),
	.td_open            = tdaio_open,
	.td_close           = tdaio_close,
	.td_queue_read      = tdaio_queue_read,
	.td_queue_write     = tdaio_queue_write,
	.td_get_parent_id   = tdaio_get_parent_id,
	.td_validate_parent = tdaio_validate_parent,
	.td_debug           = NULL,
	.td_stats           = tdaio_stats,
};
