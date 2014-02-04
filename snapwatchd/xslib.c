/*
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <xenstore.h>
#include "xslib.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <malloc.h>
#define MAXDIRBUF 4096
const int READ_SIZE = 16 * 1024;

int remove_base_watch(struct xs_handle *h)
{
	if (!xs_unwatch(h, "/vss", "vss"))
		return -EINVAL;
	return 0;
}

int register_base_watch(struct xs_handle *h)
{
	if (!xs_watch(h, "/vss", "vss"))
		return -EINVAL;
	return 0;
}

int xs_exists(struct xs_handle *h, const char *path)
{
        char **d;
        unsigned int num;
        xs_transaction_t xth;

        if ((xth = xs_transaction_start(h)) == XBT_NULL) {
                printf("unable to start xs trasanction\n");
                return 0;
        }

        d = xs_directory(h, xth, path, &num);
        xs_transaction_end(h, xth, 0);
        if (!d)
                return 0;

        free(d);
        return 1;
}

char *dirlist(struct xs_handle *h, const char *path)
{
        char **d, *p, *ptr;
        xs_transaction_t xth;
	unsigned int num, j=0, count = 0;

	if ((p = calloc(1,MAXDIRBUF))==NULL) {
		printf("unable to allocate memory\n");
		return NULL;
	}
        if ((xth = xs_transaction_start(h)) == XBT_NULL) {
                printf("unable to start xs trasanction\n");
                return p;
        }

        d = xs_directory(h, xth, path, &num);
        xs_transaction_end(h, xth, 0);
        if (!d)
                return p;

	ptr = p;	
        while(j < num) {
		ptr = p + count;
		if ((count + strlen(d[j]) + 1) > MAXDIRBUF) {
			printf("Reached max dir entry\n");
			return p;
		}
		if (j) {
			*ptr = '|';
			*ptr++;
			count++;
		}
		count += sprintf(ptr, d[j]);
		j++;
	}

        free(d);
        return p;	
}

char *getval(struct xs_handle *h, const char *path)
{
	char *p = NULL;
	xs_transaction_t xth;
	unsigned int len;

        if ((xth = xs_transaction_start(h)) == XBT_NULL) {
                printf("unable to start xs trasanction\n");
                return p;
        }
	p = xs_read(h, xth, path, &len);
	xs_transaction_end(h, xth, 0);
	return p;
}

int setval(struct xs_handle *h, const char *path, const char *val)
{
        int ret = 0;
	xs_transaction_t xth;
	unsigned int len;

        if ((xth = xs_transaction_start(h)) == XBT_NULL) {
                printf("unable to start xs trasanction\n");
                return ret;
        }
	len = strlen(val);
	ret = (xs_write(h, xth, path, val, len)? 1 : 0);
	xs_transaction_end(h, xth, 0);
	return ret;
}

int remove_xs_entry(struct xs_handle *h, char *dom_uuid, char *dom_path)
{
	char *path = NULL;
	int ret = 0;

	if (asprintf(&path, "/vss/%s/%s",dom_uuid, dom_path)==-1)
		goto out;

	if (xs_exists(h, path)) {
		if (!xs_rm(h, XBT_NULL, path)) {
			printf("Failed to remove xs entry %s\n", path);
			goto out;
		}
	}
	ret = 1;
 out:
	free(path);
	return ret;
}

int generic_remove_xs_entry(struct xs_handle *h, char *path)
{
	int ret = 0;

	if (xs_exists(h, path)) {
		if (!xs_rm(h, XBT_NULL, path)) {
			printf("Failed to remove xs entry %s\n", path);
			goto out;
		}
	}
	ret = 1;
 out:
	return ret;
}

char *
control_handle_event(struct xs_handle *h)
{
	unsigned int num;
	char **res, *path;

	res = xs_read_watch(h, &num);
	if (!res)
		return NULL;
	return res[XS_WATCH_PATH];
}

// get minimum block size for writes to the passed in file descriptor
struct int_result get_min_blk_size(int fd)
{
	long min_blk_size = 0;
	struct int_result result;
	memset(&result, 0, sizeof(result));
	
	if(ioctl(fd, BLKSSZGET, &min_blk_size) == -1)
	{
		result.result = -1;
		result.err = errno;
	}
	else
	{
		result.result = min_blk_size;		
	}

	return result;
}

// open file for direct writes
struct int_result open_file_for_write(char *path)
{
	struct int_result result;
	memset(&result, 0, sizeof(result));
	
	result.result = open( path, O_RDWR | O_DIRECT);
	if(result.result == -1)
		result.err = errno;
	return result;
}

// open file for direct reads
struct int_result open_file_for_read(char *path)
{
	struct int_result result;
	memset(&result, 0, sizeof(result));
	
	result.result = open( path, O_RDONLY | O_DIRECT);
	if(result.result == -1)
		result.err = errno;
	return result;
}

// write file by allocation memaligned buffers, which are multiples of block size
// if less, pad with spaces.
struct int_result xs_file_write(int fd, int offset, int blocksize, char* data, int length)
{
	struct int_result result;
	memset(&result, 0, sizeof(result));
	
	int newlength = length, i = 0;
	if(length % blocksize)
		newlength = length + (blocksize - length % blocksize);
	
	char *value = memalign(blocksize, newlength);
	memcpy(value, data, length);
	for(i = length; i < newlength; i++)
		value[i] = ' ';
	lseek(fd, offset, 0);	
	result.result = write(fd, value, newlength);
	if(result.result == -1)
		result.err = errno;
	free(value);
	return result;
}

// read required number of bytes in 16K chunks. 
struct xs_read_result xs_file_read(int fd, int offset, int bytesToRead, int min_block_size)
{
	struct xs_read_result result;
	memset(&result, 0, sizeof(result));
	
	char *read_value = calloc(bytesToRead + 1, 1);		
	
	lseek(fd, offset, 0);		
	int index = 0;
        int count = 0;
	char *value = memalign(min_block_size, READ_SIZE);
	while((index < bytesToRead) && (count != -1))
	{
		count = read(fd, value, READ_SIZE);
		if(count != -1)		
		{
			if(index + count > bytesToRead)
				count = bytesToRead - index;
			result.noOfBytesRead += count;
			memcpy(&read_value[index], value, count);
			index += count;
		}
		else
		{
			result.result = -1;
			result.err = errno;
		}
	}
	free(value);
	result.readString = read_value;
	return result;
}

void close_file(int fd)
{
	close(fd);
}

