/*
 * Copyright (c) 2005 Julian Chesterfield and Andrew Warfield.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <xs.h>
                                                                     
#include "tapdisk-dispatch.h"
#include "disktypes.h"

#define MSG_SIZE     4096
#define MAX_TIMEOUT  300

static int blktap_ctlfd;

struct cp_request {
	char     *cp_uuid;
	int       cp_drivertype;
};

struct lock_request {
	int       ro;
	int       enforce;
	char     *lock_uuid;
};

typedef struct driver_list_entry {
	struct blkif *blkif;
	struct driver_list_entry **pprev, *next;
} driver_list_entry_t;

int run = 0;
static struct xs_handle *xsh;
int max_timeout = MAX_TIMEOUT;

static int write_msg(int fd, int msgtype, void *ptr, void *ptr2);
static int read_msg(int fd, int msgtype, void *ptr);
static driver_list_entry_t *active_disks[MAX_DISK_TYPES];

static void
init_driver_list(void)
{
	int i;

	for (i = 0; i < MAX_DISK_TYPES; i++)
		active_disks[i] = NULL;
	return;
}

int
tapdisk_control_connected(blkif_t *blkif)
{
	int ret;
	char *path = NULL, *s = NULL, *devname = NULL;
	int major, minor;

	ret = ioctl(blktap_ctlfd, BLKTAP_IOCTL_BACKDEV_SETUP, blkif->minor);
	if (ret < 0)
		goto fail;

	ret = asprintf(&path, "%s/backdev-node", blkif->backend_path);
	if (ret < 0) {
		path = NULL;
		goto fail;
	}

	s = xs_read(xsh, XBT_NULL, path, NULL);
	if (s == NULL) {
		ret = -1;
		goto fail;
	}

	ret = sscanf(s, "%d:%d", &major, &minor);
	if (ret != 2) {
		ret = -1;
		goto fail;
	}

	ret = asprintf(&devname,"%s/%s%d", BLKTAP_DEV_DIR, BACKDEV_NAME,
		       minor);
	if (ret < 0) {
		devname = NULL;
		goto fail;
	}

	make_blktap_dev(devname, major, minor, S_IFBLK | 0600);

	free(path);
	ret = asprintf(&path, "%s/backdev-path", blkif->backend_path);
	if (ret < 0) {
		path = NULL;
		goto fail;
	}

	ret = xs_write(xsh, XBT_NULL, path, devname, strlen(devname));
	if (!ret)
		goto fail;

	ret = 0;
 out:
	free(devname);
	free(path);
	free(s);
	return ret;

 fail:
	EPRINTF("backdev setup failed [%d]\n", ret);
	goto out;
}

static int
get_new_dev(int *major, int *minor, blkif_t *blkif)
{
	domid_translate_t tr;
	int ret;
	char *devname;
	
	tr.domid = blkif->domid;
        tr.busid = (unsigned short)blkif->be_id;
	ret = ioctl(blktap_ctlfd, BLKTAP_IOCTL_NEWINTF, tr );
	
	if ( (ret <= 0)||(ret > MAX_TAP_DEV) ) {
		EPRINTF("Incorrect Dev ID [%d]\n",ret);
		return -1;
	}
	
	*minor = ret;
	*major = ioctl(blktap_ctlfd, BLKTAP_IOCTL_MAJOR, ret );
	if (*major < 0) {
		EPRINTF("Incorrect Major ID [%d]\n",*major);
		return -1;
	}

	ret = asprintf(&devname, "%s/%s%d", BLKTAP_DEV_DIR, BLKTAP_DEV_NAME,
		       *minor);
	if (ret < 0) {
		EPRINTF("get_new_dev: malloc failed\n");
		return -1;
	}

	make_blktap_dev(devname,*major,*minor, S_IFCHR | 0600);
	DPRINTF("Received device id %d and major %d, "
		"sent domid %d and be_id %d\n",
		*minor, *major, tr.domid, tr.busid);
	free(devname);
	return 0;
}

static int
get_tapdisk_pid(blkif_t *blkif)
{
	int ret;

	if ((ret = write_msg(blkif->fds[WRITE], CTLMSG_PID, blkif, NULL)) 
	    <= 0) {
		EPRINTF("Write_msg failed - CTLMSG_PID(%d)\n", ret);
		return -EINVAL;
	}

	if ((ret = read_msg(blkif->fds[READ], CTLMSG_PID_RSP, blkif))
	     <= 0) {
		EPRINTF("Read_msg failure - CTLMSG_PID(%d)\n", ret);
		return -EINVAL;
	}	
	return 1;
}

/* 
 * Look up the disk specified by path: 
 *   if found, dev points to the device string in the path
 *             type is the tapdisk driver type id
 *             blkif is the existing interface if this is a shared driver
 *             and NULL otherwise.
 *   return 0 on success, -1 on error.
 */
static int
test_path(char *path, char **dev, int *type, blkif_t **blkif)
{
	char *ptr, handle[10];
	int i, size, found = 0;

	found  = 0;
        *blkif = NULL;
	*type  = MAX_DISK_TYPES + 1;
	size   = sizeof(dtypes) / sizeof(disk_info_t *);

	if ((ptr = strstr(path, ":"))) {
		memcpy(handle, path, (ptr - path));
		*dev = ptr + 1;
		ptr = handle + (ptr - path);
		*ptr = '\0';

		for (i = 0; i < size; i++) 
			if (strncmp(handle, dtypes[i]->handle, 
                                    (ptr - path)) ==0) {
                                found = 1;
                                break;
                        }

                if (found) {
                        *type = dtypes[i]->idnum;

			if (dtypes[i]->idnum == -1)
				goto fail;
                        
                        if (dtypes[i]->single_handler == 1) {
                                /* Check whether tapdisk process 
                                   already exists */
                                if (active_disks[dtypes[i]->idnum] == NULL) 
                                        *blkif = NULL;
                                else 
                                        *blkif = active_disks[dtypes[i]
							      ->idnum]->blkif;
                        }
                        return 0;
                }
        }
 fail:
        /* Fall-through case, we didn't find a disk driver. */
        EPRINTF("Unknown blktap disk type [%s]!\n",handle);
        *dev = NULL;
        return -1;
}

static int
add_disktype(blkif_t *blkif, int type)
{
	driver_list_entry_t *entry, **pprev;

	if (type > MAX_DISK_TYPES)
		return -EINVAL;

	entry = malloc(sizeof(driver_list_entry_t));
	if (!entry)
		return -ENOMEM;

	entry->blkif = blkif;
	entry->next  = NULL;

	pprev = &active_disks[type];
	while (*pprev != NULL)
		pprev = &(*pprev)->next;

	*pprev = entry;
	entry->pprev = pprev;

	return 0;
}

static int
del_disktype(blkif_t *blkif)
{
	driver_list_entry_t *entry, **pprev;
	int type = blkif->drivertype, count = 0, close = 0;

	if (type > MAX_DISK_TYPES)
		return 1;

	pprev = &active_disks[type];
	while ((*pprev != NULL) && ((*pprev)->blkif != blkif))
		pprev = &(*pprev)->next;

	if ((entry = *pprev) == NULL) {
		EPRINTF("DEL_DISKTYPE: No match\n");
		return 1;
	}

	*pprev = entry->next;
	if (entry->next)
		entry->next->pprev = pprev;

	DPRINTF("DEL_DISKTYPE: Freeing entry\n");
	free(entry);

	/* Caller should close() if no single controller, or list is empty. */
	return (!dtypes[type]->single_handler || (active_disks[type] == NULL));
}

static int
write_timeout(int fd, char *buf, size_t len, int timeout)
{
	fd_set writefds;
	struct timeval tv;
	int offset;
	int ret;

	tv.tv_sec = timeout;
	tv.tv_usec = 0;

	offset = 0;

	while (offset < len) {
		FD_ZERO(&writefds);
		FD_SET(fd, &writefds);
		/* we don't bother reinitializing tv. at worst, it will wait a
		 * bit more time than expected. */
		ret = select(fd + 1, NULL, &writefds, NULL, &tv);
		if (ret == -1)
			break;
		else if (FD_ISSET(fd, &writefds)) {
			ret = write(fd, buf + offset, len - offset);
			if (ret <= 0) break;
			offset += ret;
		} else
			break;
	}
	return (offset == len) ? len : 0;
}

static int
write_msg(int fd, int msgtype, void *ptr, void *ptr2)
{
	blkif_t *blkif;
	blkif_info_t *blk;
	msg_hdr_t *msg;
	msg_params_t *msg_p;
	msg_newdev_t *msg_dev;
	msg_cp_t *msg_cp;
	msg_lock_t *msg_lock;
	char *buf, *path;
	int msglen, len, ret;
	image_t *image, *img;
	uint32_t seed;
	struct cp_request *req;
	struct lock_request *lock_req;

	blkif = (blkif_t *)ptr;
	blk = blkif->info;
	image = blkif->prv;
	len = 0;

	switch (msgtype)
	{
	case CTLMSG_PARAMS:
		path = (char *)ptr2;
		DPRINTF("Write_msg called: CTLMSG_PARAMS, sending [%s]\n",
			path);

		msglen = sizeof(msg_hdr_t) + sizeof(msg_params_t) +
			strlen(path) + 1;
		buf = calloc(1, msglen);
		if (!buf)
			return -1;

		/*Assign header fields*/
		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_PARAMS;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;

		msg->cookie = blkif->cookie;
		DPRINTF("Generated cookie, %d\n",blkif->cookie);

		/*Copy blk->params to msg*/
		msg_p           = (msg_params_t *)(buf + sizeof(msg_hdr_t));
		msg_p->readonly = blk->readonly;
		msg_p->storage  = blk->storage;

		msg_p->path_off = sizeof(msg_hdr_t) + sizeof(msg_params_t);
		msg_p->path_len = strlen(path) + 1;
		memcpy(&buf[msg_p->path_off], path, msg_p->path_len);

		break;

	case CTLMSG_NEWDEV:
		DPRINTF("Write_msg called: CTLMSG_NEWDEV\n");

		msglen = sizeof(msg_hdr_t) + sizeof(msg_newdev_t);
		buf = calloc(1, msglen);
		if (!buf)
			return -1;
		
		/*Assign header fields*/
		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_NEWDEV;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;
		msg->cookie = blkif->cookie;
		
		msg_dev = (msg_newdev_t *)(buf + sizeof(msg_hdr_t));
		msg_dev->devnum = blkif->minor;
		msg_dev->domid = blkif->domid;

		break;

	case CTLMSG_CLOSE:
		DPRINTF("Write_msg called: CTLMSG_CLOSE\n");

		msglen = sizeof(msg_hdr_t);
		buf = calloc(1, msglen);
		if (!buf)
			return -1;
		
		/*Assign header fields*/
		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_CLOSE;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;
		msg->cookie = blkif->cookie;
		
		break;

	case CTLMSG_PID:
		DPRINTF("Write_msg called: CTLMSG_PID\n");

		msglen = sizeof(msg_hdr_t);
		buf = calloc(1, msglen);
		if (!buf)
			return -1;
		
		/*Assign header fields*/
		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_PID;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;
		msg->cookie = blkif->cookie;
		
		break;

	case CTLMSG_CHECKPOINT:
		DPRINTF("Write_msg called: CTLMSG_CHECKPOINT\n");
		req = (struct cp_request *)ptr2;
		msglen = sizeof(msg_hdr_t) + sizeof(msg_cp_t) + 
			strlen(req->cp_uuid) + 1;
		buf = calloc(1, msglen);
		if (!buf)
			return -1;

		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_CHECKPOINT;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;
		msg->cookie = blkif->cookie;

		msg_cp = (msg_cp_t *)(buf + sizeof(msg_hdr_t));
		msg_cp->cp_drivertype = req->cp_drivertype;
		msg_cp->cp_uuid_off = sizeof(msg_hdr_t) + sizeof(msg_cp_t);
		msg_cp->cp_uuid_len = strlen(req->cp_uuid) + 1;
		memcpy(&buf[msg_cp->cp_uuid_off], 
		       req->cp_uuid, msg_cp->cp_uuid_len);
		
		break;

	case CTLMSG_LOCK:
		DPRINTF("Write_msg called: CTLMSG_LOCK\n");
		lock_req = (struct lock_request *)ptr2;
		msglen = sizeof(msg_hdr_t) + sizeof(msg_lock_t) + 
			strlen(lock_req->lock_uuid) + 1;
		buf = calloc(1, msglen);
		if (!buf)
			return -1;

		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_LOCK;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;
		msg->cookie = blkif->cookie;

		msg_lock = (msg_lock_t *)(buf + sizeof(msg_hdr_t));
		msg_lock->ro = lock_req->ro;
		msg_lock->enforce = lock_req->enforce;
		msg_lock->uuid_off = sizeof(msg_hdr_t) + 
			sizeof(msg_lock_t);
		msg_lock->uuid_len = strlen(lock_req->lock_uuid) + 1;
		memcpy(&buf[msg_lock->uuid_off], 
		       lock_req->lock_uuid, msg_lock->uuid_len);

		break;

	case CTLMSG_PAUSE:
		DPRINTF("Write_msg called: CTLMSG_PAUSE\n");

		buf = calloc(1, sizeof(msg_hdr_t));
		if (!buf)
			return -1;
		
		msglen = sizeof(msg_hdr_t);
		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_PAUSE;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;
		msg->cookie = blkif->cookie;

		break;

	case CTLMSG_RESUME:
		DPRINTF("Write_msg called: CTLMSG_RESUME\n");

		buf = calloc(1, sizeof(msg_hdr_t));
		if (!buf)
			return -1;

		msglen = sizeof(msg_hdr_t);
		msg = (msg_hdr_t *)buf;
		msg->type = CTLMSG_RESUME;
		msg->len = msglen;
		msg->drivertype = blkif->drivertype;
		msg->cookie = blkif->cookie;

		break;
		
	default:
		return -1;
	}

	/*Now send the message*/
	if (write_timeout(fd, buf, msglen, max_timeout) != msglen) {
		DPRINTF("Write failed: (%d)\n", errno);
		free(buf);
		return -EIO;
	}

	free(buf);
	return msglen;
}

static int
read_timeout(int fd, char *buf, size_t len, int timeout)
{
	fd_set readfds;
	struct timeval tv;
	int offset;
	int ret;

	tv.tv_sec = timeout;
	tv.tv_usec = 0;

	offset = 0;

	while (offset < len) {
		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);
		/* we don't bother reinitializing tv. at worst, it will wait a
		 * bit more time than expected. */
		ret = select(fd + 1, &readfds, NULL, NULL, &tv);
		if (ret == -1)
			break;
		else if (FD_ISSET(fd, &readfds)) {
			ret = read(fd, buf + offset, len - offset);
			if (ret <= 0) break;
			offset += ret;
		} else
			break;
	}
	return (offset == len) ? len : 0;
}

static int
read_msg(int fd, int msgtype, void *ptr)
{
	blkif_t *blkif;
	blkif_info_t *blk;
	msg_hdr_t msg;
	int ret;
	image_t *image;


	blkif = (blkif_t *)ptr;
	blk = blkif->info;
	image = blkif->prv;

	ret = read_timeout(fd, (char *) &msg, sizeof(msg_hdr_t), max_timeout);
	if (ret == 0)
		return -EIO;

	switch (msg.type) {
	case CTLMSG_IMG:
	{
		image_t img;
		ret = read_timeout(fd, (void *) &img,
				   sizeof(image_t), max_timeout);
		if (ret == 0)
			return -EIO;
		image->size = img.size;
		image->secsize = img.secsize;
		image->info = img.info;

		DPRINTF("Received CTLMSG_IMG: %llu, %lu, %u\n",
			image->size, image->secsize, image->info);
		if(msgtype != CTLMSG_IMG) ret = 0;
		break;
	}
	case CTLMSG_IMG_FAIL:
		DPRINTF("Received CTLMSG_IMG_FAIL, "
			"unable to open image\n");
		ret = 0;
		break;
			
	case CTLMSG_NEWDEV_RSP:
		DPRINTF("Received CTLMSG_NEWDEV_RSP\n");
		if(msgtype != CTLMSG_NEWDEV_RSP) ret = 0;
		break;
		
	case CTLMSG_NEWDEV_FAIL:
		DPRINTF("Received CTLMSG_NEWDEV_FAIL\n");
		ret = 0;
		break;
		
	case CTLMSG_CLOSE_RSP:
		DPRINTF("Received CTLMSG_CLOSE_RSP\n");
		if (msgtype != CTLMSG_CLOSE_RSP) ret = 0;
		break;

	case CTLMSG_PID_RSP:
		DPRINTF("Received CTLMSG_PID_RSP\n");
		if (msgtype != CTLMSG_PID_RSP) ret = 0;
		else {
			msg_pid_t msg_pid;
			ret = read_timeout(fd, (char *) &msg_pid,
					   sizeof(msg_pid_t), max_timeout);
			if (ret == 0)
				return -EIO;
			blkif->tappid = msg_pid.pid;
			DPRINTF("\tPID: [%d]\n",blkif->tappid);
		}
		break;

	case CTLMSG_CHECKPOINT_RSP:
		DPRINTF("Received CTLMSG_CHECKPOINT_RSP\n");
		if (msgtype != CTLMSG_CHECKPOINT_RSP) 
			ret = 0;
		else {
			int err;
			ret = read_timeout(fd, (char *) &err,
					   sizeof(int), max_timeout);
			if (ret == 0)
				return -EIO;
			blkif->err = err;
		}
		break;

	case CTLMSG_LOCK_RSP:
		DPRINTF("Received CTLMSG_LOCK_RSP\n");
		if (msgtype != CTLMSG_LOCK_RSP)
			ret = 0;
		else {
			int err;
			ret = read_timeout(fd, (char *) &err,
					   sizeof(int), max_timeout);
			if (ret == 0)
				return -EIO;
			blkif->err = err;
		}
		break;

	case CTLMSG_PAUSE_RSP:
		DPRINTF("Received CTLMSG_PAUSE_RSP\n");
		if (msgtype != CTLMSG_PAUSE_RSP)
			ret = 0;
		else {
			int err;
			ret = read_timeout(fd, (char *) &err,
					   sizeof(int), max_timeout);
			if (ret == 0)
				return -EIO;
			blkif->err = err;
		}
		break;
				
	case CTLMSG_RESUME_RSP:
		DPRINTF("Received CTLMSG_RESUME_RSP\n");
		if (msgtype != CTLMSG_RESUME_RSP)
			ret = 0;
		else {
			int err;
			ret = read_timeout(fd, (char *) &err,
					   sizeof(int), max_timeout);
			if (ret == 0)
				return -EIO;
			blkif->err = err;
		}
		break;

	default:
		DPRINTF("UNKNOWN MESSAGE TYPE RECEIVED\n");
		ret = 0;
		break;
	}

	return ret;

}

static int
launch_tapdisk(char *wrctldev, char *rdctldev)
{
	char *argv[] = { "tapdisk", wrctldev, rdctldev, NULL };
	pid_t child;

	if ((child = fork()) < 0)
		return -1;

	if (!child) {
		int i;
		for (i = 0 ; i < sysconf(_SC_OPEN_MAX) ; i++)
			if (i != STDIN_FILENO &&
			    i != STDOUT_FILENO &&
			    i != STDERR_FILENO)
				close(i);

		execvp("tapdisk", argv);
		_exit(1);
	} else {
		pid_t got;
		do {
			got = waitpid(child, NULL, 0);
		} while (got != child);
	}
	return 0;
}

static int
open_ctrl_socket(char *devname)
{
	int ret;
	int ipc_fd;
	fd_set socks;
	struct timeval timeout;

	if (devname == NULL)
		return -1;

	if (mkdir(BLKTAP_CTRL_DIR, 0755) == 0)
		DPRINTF("Created %s directory\n", BLKTAP_CTRL_DIR);

	ret = mkfifo(devname, S_IRWXU | S_IRWXG | S_IRWXO);
	if (ret) {
		if (errno == EEXIST) {
			/*
			 * Remove fifo since it may have data from
			 * it's previous use --- earlier invocation
			 * of tapdisk may not have read all messages.
			 */
			ret = unlink(devname);
			if (ret) {
				EPRINTF("ERROR: unlink(%s) failed (%d)\n",
					devname, errno);
				return -1;
			}

			ret = mkfifo(devname, S_IRWXU | S_IRWXG | S_IRWXO);
		}
		if (ret) {
			EPRINTF("ERROR: pipe failed (%d)\n", errno);
			return -1;
		}
	}

	ipc_fd = open(devname,O_RDWR|O_NONBLOCK);

	if (ipc_fd < 0) {
		EPRINTF("FD open failed\n");
		return -1;
	}

	return ipc_fd;
}

int
tapdisk_control_new(blkif_t *blkif)
{
	image_t *image;
	blkif_info_t *blk;
	blkif_t *exist = NULL;
	int ret, major, minor, type;
	char *rdctldev, *wrctldev, *ptr;
	static uint16_t next_cookie = 0;

	DPRINTF("Received a poll for a new vbd\n");

	if (((blk = blkif->info)) && blk->params) {
		if (get_new_dev(&major, &minor, blkif) < 0)
			return -1;

		if (test_path(blk->params, &ptr, &type, &exist) != 0) {
                        EPRINTF("Error in blktap device string (%s)\n",
                                blk->params);
			goto fail;
                }

		blkif->drivertype = type;
		blkif->cookie = next_cookie++;

		if (!exist) {
			DPRINTF("Process does not exist:\n");
			ret = asprintf(&rdctldev, "%s/tapctrlread%d",
				       BLKTAP_CTRL_DIR, minor);
			if (ret < 0)
				rdctldev = NULL;
			blkif->fds[READ] = open_ctrl_socket(rdctldev);

			ret = asprintf(&wrctldev, "%s/tapctrlwrite%d",
				       BLKTAP_CTRL_DIR, minor);
			if (ret < 0)
				wrctldev = NULL;
			blkif->fds[WRITE] = open_ctrl_socket(wrctldev);
			
			if (blkif->fds[READ] == -1 ||
			    blkif->fds[WRITE] == -1) {
				EPRINTF("new blkif: socket failed %d %d\n",
					blkif->fds[READ], blkif->fds[WRITE]);
				if (blkif->fds[READ] != -1) {
					close(blkif->fds[READ]);
					blkif->fds[READ] = -1;
				}
				if (blkif->fds[WRITE] != -1) {
					close(blkif->fds[WRITE]);
					blkif->fds[READ] = -1;
				}
				free(rdctldev);
				free(wrctldev);
				goto fail;
			}

			/*launch the new process*/
			DPRINTF("Launching process, [wr rd] = [%s %s]\n", 
				wrctldev, rdctldev);
			if (launch_tapdisk(wrctldev, rdctldev) == -1) {
				EPRINTF("Unable to launch tapdisk, [wr rd] = "
					"[%s %s]\n", wrctldev, rdctldev);
				close(blkif->fds[READ]);
				close(blkif->fds[WRITE]);
				free(rdctldev);
				free(wrctldev);
				goto fail;
			}
			DPRINTF("process launched\n");
			free(rdctldev);
			free(wrctldev);
		} else {
			DPRINTF("Process exists!\n");
			blkif->fds[READ] = exist->fds[READ];
			blkif->fds[WRITE] = exist->fds[WRITE];
		}

		if (add_disktype(blkif, type))
			goto fail;

		blkif->major = major;
		blkif->minor = minor;

		image = (image_t *)malloc(sizeof(image_t));
		if (!image)
			goto fail;

		blkif->prv = (void *)image;
		blkif->ops = &tapdisk_ops;

		/*Retrieve the PID of the new process*/
		if (get_tapdisk_pid(blkif) <= 0) {
			EPRINTF("Unable to contact disk process\n");
			goto fail;
		}

		/* exempt tapdisk from flushing */
		if (setpriority(PRIO_PROCESS,
				blkif->tappid, PRIO_SPECIAL_IO)) {
			EPRINTF("Unable to prioritize tapdisk proc\n");
			goto fail;
		} 

		/* Both of the following read and write calls will block up to 
		 * max_timeout val*/
		if (write_msg(blkif->fds[WRITE], CTLMSG_PARAMS, blkif, ptr) 
		    <= 0) {
			EPRINTF("Write_msg failed - CTLMSG_PARAMS\n");
			goto fail;
		}

		if (read_msg(blkif->fds[READ], CTLMSG_IMG, blkif) <= 0) {
			EPRINTF("Read_msg failure - CTLMSG_IMG\n");
			goto fail;
		}

	} else return -1;

	return 0;
fail:
	ioctl(blktap_ctlfd, BLKTAP_IOCTL_FREEINTF, minor);
	return -EINVAL;
}

int
tapdisk_control_map(blkif_t *blkif)
{
	DPRINTF("Received a poll for a new devmap\n");
	if (write_msg(blkif->fds[WRITE], CTLMSG_NEWDEV, blkif, NULL) <= 0) {
		EPRINTF("Write_msg failed - CTLMSG_NEWDEV\n");
		return -EINVAL;
	}

	if (read_msg(blkif->fds[READ], CTLMSG_NEWDEV_RSP, blkif) <= 0) {
		EPRINTF("Read_msg failed - CTLMSG_NEWDEV_RSP\n");
		return -EINVAL;
	}
	DPRINTF("Exiting map_new_blktapctrl\n");

	return blkif->minor - 1;
}

int
tapdisk_control_unmap(blkif_t *blkif)
{
	DPRINTF("Unmapping vbd\n");

	if (write_msg(blkif->fds[WRITE], CTLMSG_CLOSE, blkif, NULL) <= 0) {
		EPRINTF("Write_msg failed - CTLMSG_CLOSE\n");
		return -EINVAL;
	}

	if (del_disktype(blkif)) {
		close(blkif->fds[WRITE]);
		close(blkif->fds[READ]);
	}

	return 0;
}

int
tapdisk_control_checkpoint(blkif_t *blkif, char *cp_request)
{
	char    *path;
	int      drivertype;
	blkif_t *tmp = NULL;
	struct cp_request req;

	blkif->err = 0;

	DPRINTF("Creating checkpoint %s\n", cp_request);
	if (test_path(cp_request, &path, &drivertype, &tmp) == -1) {
		EPRINTF("invalid checkpoint request\n");
		return -EINVAL;
	}

	req.cp_uuid = path;
	req.cp_drivertype = drivertype;

	if (write_msg(blkif->fds[WRITE], 
		      CTLMSG_CHECKPOINT, blkif, &req) <= 0) {
		EPRINTF("Write_msg failed - CTLMSG_CHECKPOINT\n");
		return -EIO;
	}

	if (read_msg(blkif->fds[READ], CTLMSG_CHECKPOINT_RSP, blkif) <= 0) {
		EPRINTF("Read_msg failed - CTLMSG_CHECKPOINT_RSP\n");
		return -EIO;
	}

	return blkif->err;
}

int
tapdisk_control_lock(blkif_t *blkif, char *lock, int enforce)
{
	int len, err = 0;
	char *tmp, rw;
	struct lock_request req;

	/* not locking... no need to signal tapdisk */
	if (!strcmp(lock, "nil"))
		return 0;

	memset(&req, 0, sizeof(struct lock_request));

	tmp = strchr(lock, ':');
	if (!tmp)
		return -EINVAL;

	rw = *(tmp + 1);
	if (rw == 'r')
		req.ro = 1;
	else if (rw != 'w')
		return -EINVAL;
	
	req.lock_uuid = malloc(tmp - lock + 1);
	if (!req.lock_uuid)
		return -ENOMEM;
	
	memcpy(req.lock_uuid, lock, tmp - lock);
	req.lock_uuid[tmp - lock] = '\0';
	req.enforce = enforce;

	if (write_msg(blkif->fds[WRITE], CTLMSG_LOCK, blkif, &req) <= 0) {
		EPRINTF("Write_msg failed - CTLMSG_LOCK\n");
		err = -EIO;
	}

	if (read_msg(blkif->fds[READ], CTLMSG_LOCK_RSP, blkif) <= 0) {
		EPRINTF("Read_msg failed - CTLMSG_LOCK_RSP\n");
		err = -EIO;
	}

	free(req.lock_uuid);
	return (err ? err : blkif->err);
}

int tapdisk_control_pause(blkif_t *blkif)
{
	if (write_msg(blkif->fds[WRITE], CTLMSG_PAUSE, blkif, NULL) <= 0) {
		DPRINTF("Write_msg failed - CTLMSG_PAUSE\n");
		return -EIO;
	}

	if (read_msg(blkif->fds[READ], CTLMSG_PAUSE_RSP, blkif) <= 0) {
		DPRINTF("Read_msg failed - CTLMSG_PAUSE_RSP\n");
		return -EIO;
	}

	return blkif->err;
}

int tapdisk_control_resume(blkif_t *blkif)
{
	if (write_msg(blkif->fds[WRITE], CTLMSG_RESUME, blkif, NULL) <= 0) {
		DPRINTF("Write_msg failed - CTLMSG_RESUME\n");
		return -EIO;
	}

	if (read_msg(blkif->fds[READ], CTLMSG_RESUME_RSP, blkif) <= 0) {
		DPRINTF("Read_msg failed - CTLMSG_RESUME_RSP\n");
		return -EIO;
	}

	return blkif->err;
}

void
tapdisk_control_start(void)
{
	++run;
}

void
tapdisk_control_stop(void)
{
	--run;
}

int
main(int argc, char **argv)
{
	int ret;
	char buf[128];
	fd_set readfds;
	char *xs_path, *uuid, *devname;

	daemon(0, 0);

	snprintf(buf, sizeof(buf), "TAPDISK-CONTROL[%d]", getpid());
	openlog(buf, LOG_CONS | LOG_ODELAY, LOG_DAEMON);

	if (argc != 3) {
		DPRINTF("usage: tapdisk-control <path> <tapdisk-uuid>\n");
		return -EINVAL;
	}

	if (asprintf(&devname, "%s/%s0", 
		     BLKTAP_DEV_DIR, BLKTAP_DEV_NAME) == -1) {
		EPRINTF("failed to open control dev %s\n", devname);
		return -EINVAL;
	}

	blktap_ctlfd = open(devname, O_RDWR);
	if (blktap_ctlfd == -1) {
		EPRINTF("%s open failed\n", devname);
		free(devname);
		return -EINVAL;
	}

	xs_path = argv[1];
	uuid    = argv[2];

	init_driver_list();
	tapdisk_control_start();

	xsh = xs_daemon_open();
	if (!xsh) {
		EPRINTF("%s: failed to connect to xs_daemon\n", xs_path);
		goto out;
	}

	ret = add_control_watch(xsh, xs_path, uuid);
	if (ret)
		goto out;

	DPRINTF("controlling %s, uuid: %s\n", xs_path, uuid);

	while (run) {
		FD_ZERO(&readfds);
		FD_SET(xs_fileno(xsh), &readfds);

		ret = select(xs_fileno(xsh) + 1, &readfds, NULL, NULL, NULL);

		if (FD_ISSET(xs_fileno(xsh), &readfds))
			ret = tapdisk_control_handle_event(xsh, uuid);
	}

 out:
	free(devname);
	close(blktap_ctlfd);
	if (xsh) {
		remove_control_watch(xsh, xs_path);
		xs_daemon_close(xsh);
	}

	DPRINTF("exiting\n");
	closelog();
	return 0;
}

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
