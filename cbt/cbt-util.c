/*
 * Copyright (c) 2017, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>

#include "cbt-util.h"
#include "cbt-util-priv.h"

int cbt_util_create(int , char **);
int cbt_util_set(int , char **);
int cbt_util_get(int , char **);
int cbt_util_coalesce(int , char **);

struct command commands[] = {
	{ .name = "create", .func = cbt_util_create},
	{ .name = "set", .func = cbt_util_set},
	{ .name = "get", .func = cbt_util_get},
	{ .name = "coalesce", .func = cbt_util_coalesce},
};

#define print_commands()					\
	do {							\
		int i, n;					\
		n = sizeof(commands) / sizeof(struct command);	\
		printf("COMMAND := { ");			\
		printf("%s", commands[0].name);			\
		for (i = 1; i < n; i++)				\
			printf(" | %s", commands[i].name);	\
		printf(" }\n");					\
	} while (0)

int
malloc_cbt_log_data(struct cbt_log_data **log_data)
{
	int err = 0;

	*log_data = malloc(sizeof(struct cbt_log_data));
	if (!*log_data) {
		fprintf(stderr, "Failed to allocate memory for CBT log\n");
		err = -ENOMEM;
	}
	else
		(*log_data)->bitmap = NULL;

	return err;
}

int
file_open(FILE **f, char * name, char *mode)
{
	int err = 0;

	*f = fopen(name, mode);
	if (*f == NULL) {
		fprintf(stderr, "Failed to open log file %s. %s\n",
										name, strerror(errno));
		err = -errno;
	}

	return err;
}

int
read_cbt_metadata(char *name, FILE *f, struct cbt_log_metadata *log_meta)
{
	int err = 0, ret;

	ret = fread(log_meta, sizeof(struct cbt_log_metadata), 1, f);

	if (!ret) {
		fprintf(stderr, "Failed to read CBT metadata from file %s\n", name);
		err = -EIO;
	}

	return err;
}

int
allocate_cbt_bitmap(struct cbt_log_data *log_data)
{
	int err = 0;

	uint64_t bmsize = bitmap_size(log_data->metadata.size);
	log_data->bitmap = malloc(bmsize);
	if (!log_data->bitmap) {
		fprintf(stderr, "Failed to allocate memory for bitmap buffer\n");
		err = -ENOMEM;
	}

	return err;
}

int
read_cbt_bitmap(FILE *f, struct cbt_log_data *log_data)
{
	int err = 0, ret;
	uint64_t bmsize = bitmap_size(log_data->metadata.size);

	ret = fread(log_data->bitmap, bmsize, 1, f);

	if (!ret) {
		fprintf(stderr, "Failed to read bitmap\n");
		err = -EIO;
	}

	return err;
}

int
cbt_util_get(int argc, char **argv)
{
	char *name, uuid_str[37], *buf;
	int err, c, ret;
	int parent, child, flag, size, bitmap;
	FILE *f = NULL;

	err			= 0;
	name		= NULL;
	parent		= 0;
	child		= 0;
	flag		= 0;
	size		= 0;
	buf			= NULL;
	bitmap		= 0;

	if (!argc || !argv)
		goto usage;

	/* Make sure we start from the start of the args */
	optind = 1;

	while ((c = getopt(argc, argv, "n:pcfsbh")) != -1) {
		switch (c) {
			case 'n':
				name = optarg;
				break;
			case 'p':
				parent = 1;
				break;
			case 'c':
				child = 1;
				break;
			case 'f':
				flag = 1;
				break;
			case 's':
				size = 1;
				break;
			case 'b':
				bitmap = 1;
				break;
			case 'h':
			default:
				goto usage;
		}
	}

	// Exactly one of p, c, f or b must be queried for
	if (!name || (parent + child + flag + size + bitmap != 1))
		goto usage;

	struct cbt_log_metadata *log_meta = malloc(sizeof(struct cbt_log_metadata));
	if (!log_meta) {
		fprintf(stderr, "Failed to allocate memory for CBT log metadata\n");
		err = -ENOMEM;
		goto error;
	}

	f = fopen(name, "r");
	if (f == NULL) {
		fprintf(stderr, "Failed to open log file %s. %s\n",
										name, strerror(errno));
		err = -errno;
		goto error;
	}

	ret = fread(log_meta, sizeof(struct cbt_log_metadata), 1, f);

	if (!ret) {
		fprintf(stderr, "Failed to read CBT metadata from file %s\n", name);
		err = -EIO;
		goto error;
	}

	if (parent) {
		uuid_unparse(log_meta->parent, uuid_str);
		printf("%s\n", uuid_str);
	}
	else if (child) {
		uuid_unparse(log_meta->child, uuid_str);
		printf("%s\n", uuid_str);
	} else if(flag) {
		printf("%d\n", log_meta->consistent);
	} else if(size) {
		printf("%"PRIu64"\n", log_meta->size);
	}
	else {
		uint64_t bmsize = bitmap_size(log_meta->size);
		buf = malloc(bmsize);
		if (!buf) {
			fprintf(stderr, "Failed to allocate memory for bitmap buffer\n");
			err = -ENOMEM;
			goto error;
		}

		ret = fread(buf, bmsize, 1, f);

		if (!ret) {
			fprintf(stderr, "Failed to read bitmap from file %s\n", name);
			err = -EIO;
			goto error;
		}
		else {
			DPRINTF("Read %"PRIu64" bytes from bitmap region for file %s\n",
						bmsize, name);
		}

		fwrite(buf, bmsize, 1, stdout);
	}

error:
	if(log_meta)
		free(log_meta);
	if(buf)
		free(buf);
	if(f)
		fclose(f);
	return err;

usage:
	printf("cbt-util get: Read field from log file\n\n");
	printf("Options:\n");
	printf(" -n name\tName of log file\n");
	printf("[-p]\t\tPrint parent log file UUID\n");
	printf("[-c]\t\tPrint child log file UUID\n");
	printf("[-f]\t\tPrint consistency flag\n");
	printf("[-s]\t\tPrint size of disk in bytes\n");
	printf("[-b]\t\tPrint bitmap contents\n");
	printf("[-h]\t\thelp\n");

	return -EINVAL;
}


int 
cbt_util_set(int argc, char **argv)
{
	char *name, *parent, *child, *buf;
	int err, c, consistent, flag = 0, ret; 
	FILE *f = NULL;
	uint64_t size, bmsize, old_bmsize;

	err 		= 0;
	name 		= NULL;
	parent		= NULL;
	child		= NULL;
	buf			= NULL;
	size		= 0;
	bmsize		= 0; 
	old_bmsize 	= 0;

	if (!argc || !argv)
		goto usage;

	while ((c = getopt(argc, argv, "n:p:c:f:s:h")) != -1) {
		switch (c) {
			case 'n':
				name = optarg;
				break;
			case 'p':
				parent = optarg;
				break;
			case 'c':
				child = optarg;
				break;
			case 'f':
				flag = 1;
				consistent = atoi(optarg);
				break;
			case 's':
				size = strtoull(optarg, NULL, 10);
				break;
			case 'h':
			default:
				goto usage;
		}
	}

	if (!name || !(parent || child || flag || size))
		goto usage;

	struct cbt_log_metadata *log_meta = malloc(sizeof(struct cbt_log_metadata));
	if (!log_meta) {
		fprintf(stderr, "Failed to allocate memory for CBT log metadata\n");
		err = -ENOMEM;
		goto error;
	}

	f = fopen(name, "r+");
	if (f == NULL) {
		fprintf(stderr, "Failed to open log file %s. %s\n",
											name, strerror(errno));
		err = -errno;
		goto error;
	}
	
	ret = fread(log_meta, sizeof(struct cbt_log_metadata), 1, f);

	if (!ret) {
		fprintf(stderr, "Failed to read CBT metadata from file %s\n", name);
		err = -EIO;
		goto error;
	}

	if (parent) {
		uuid_parse(parent, log_meta->parent);
	}

	if (child) {
		uuid_parse(child, log_meta->child);
	}

	if (flag) {
		log_meta->consistent = consistent;
	}

	if (size) {
		if (size < log_meta->size) {
			fprintf(stderr, "Size smaller than current file size (%"PRIu64")\n",
									log_meta->size);
			err = -EINVAL;
			goto error;
		}

		bmsize = bitmap_size(size);
		old_bmsize = bitmap_size(log_meta->size);
		buf = malloc(bmsize);
		if (!buf) {
			fprintf(stderr, "Failed to allocate memory for bitmap buffer\n");
			err = -ENOMEM;
			goto error;
		}

		ret = fread(buf, old_bmsize, 1, f);
		if (!ret) {
			fprintf(stderr, "Failed to read bitmap from file %s\n", name);
			err = -EIO;
			goto error;
		}

		memset(buf + old_bmsize, 0, bmsize - old_bmsize);
		// Set file pointer to start of bitmap area
		ret = fseek(f, sizeof(struct cbt_log_metadata), SEEK_SET);
		if(ret < 0) {
			fprintf(stderr, "Failed to seek to start of bitmap in file %s. %s\n", 
													name, strerror(errno));
			err = -errno;
			goto error;
		}

		ret = fwrite(buf, bmsize, 1, f);
		if (!ret) {
			fprintf(stderr, "Failed to write CBT bitmap to file %s\n", name);
			err = -EIO;
		}
                                                                                
		log_meta->size = size;
	}

	// Rewind pointer to start of file and rewrite data
	ret = fseek(f, 0, SEEK_SET);
	if(ret < 0) {
		fprintf(stderr, "Failed to seek to start of file %s. %s\n", 
													name, strerror(errno));
		err = -errno;
		goto error;
	}

	ret = fwrite(log_meta, sizeof(struct cbt_log_metadata), 1, f);
	if (!ret) {
		fprintf(stderr, "Failed to write CBT metadata to file %s\n", name);
		err = -EIO;
	}

error:
	if(log_meta)
		free(log_meta);
	if(f)
		fclose(f);
	return err;

usage:
	printf("cbt-util set: Set field in log file\n\n");
	printf("Options:\n");
	printf(" -n name\tName of log file\n");
	printf("[-p parent]\tParent log file UUID\n");
	printf("[-c child]\tChild log file UUID\n");
	printf("[-f 0|1]\tConsistency flag\n");
	printf("[-s size]\tSize of the disk in bytes\n");
	printf("[-h]\t\thelp\n");

	return -EINVAL;
}

int 
cbt_util_create(int argc, char **argv)
{
	char *name;
	int err, c, ret;
	FILE *f = NULL; 
	uint64_t size, bitmap_sz;

	err  = 0;
	name = NULL;
	size = 0;

	if (!argc || !argv)
		goto usage;

	/* Make sure we start from the start of the args */
	optind = 1;

	while ((c = getopt(argc, argv, "n:s:h")) != -1) {
		switch (c) {
			case 'n':
				name = optarg;
				break;
			case 's':
				size = strtoull(optarg, NULL, 10);
				break;
			case 'h':
			default:
				goto usage;
		}
	}

	if (!name || !size) 
		goto usage;

	/* Initialise metadata */
	struct cbt_log_data *log_data = malloc(sizeof(struct cbt_log_data));
	if (!log_data) {
		err = -ENOMEM;
		goto error;
	}

	uuid_clear(log_data->metadata.parent);
	uuid_clear(log_data->metadata.child);
	log_data->metadata.consistent = 0;
	log_data->metadata.size = size;

	DPRINTF("Creating new log file %s of size %"PRIu64"\n", name, size);

	bitmap_sz = bitmap_size(size);
	log_data->bitmap = (char*)malloc(bitmap_sz);
	if (!log_data->bitmap) {
		err = -ENOMEM;
		goto error;
	}

	memset(log_data->bitmap, 0, bitmap_sz);

	f = fopen(name, "w+");
	if (f == NULL) {
		fprintf(stderr, "Failed to open log file %s. %s\n",
											name, strerror(errno));
		err = -errno;
		goto error;
	}

	ret = fwrite(&log_data->metadata, sizeof(struct cbt_log_metadata), 1, f);
	if (!ret) {
		fprintf(stderr, "Failed to write metadata to log file %s\n", name);
		err = -EIO;
		goto error;
	}

	ret = fwrite(log_data->bitmap, bitmap_sz, 1, f);
	if (!ret) {
		fprintf(stderr, "Failed to write bitmap to log file %s\n", name);
		err = -EIO;
		goto error;
	}
	else {
		DPRINTF("Bitmap area of %"PRIu64" bytes initialised\n", bitmap_sz);
	}
	

error:
	if(log_data) {
		if(log_data->bitmap) {
			free(log_data->bitmap);
		}
		free(log_data);
	}
	if(f)
		fclose(f);

	return err;

usage:
	printf("cbt-util create: Create new CBT metadata log with default values\n\n");
	printf("Options:\n\t-n Log file name\n\t-s Num blocks in bitmap\n"
			"\t[-h help]\n");

	return -EINVAL;
}

int
cbt_util_coalesce(int argc, char **argv)
{
	char *parent, *child, *pbuf, *cbuf;
	int err, c, ret;
	FILE *fparent = NULL, *fchild = NULL;;
	struct cbt_log_data *parent_log, *child_log;
	uint64_t size;

	parent = NULL;
	child = NULL;
	parent_log = NULL;
	child_log = NULL;
	pbuf = NULL;
	cbuf = NULL;

	if (!argc || !argv)
		goto usage;

	/* Make sure we start from the start of the args */
	optind = 1;

	while ((c = getopt(argc, argv, "p:c:h")) != -1) {
		switch (c) {
			case 'p':
				parent = optarg;
				break;
			case 'c':
				child = optarg;
				break;
			case 'h':
			default:
				goto usage;
		}
	}

	if (!parent || !child)
		goto usage;

	// Open parent log in r/o mode
	err = file_open(&fparent, parent, "r");
	if (err)
		goto error;

	err = malloc_cbt_log_data(&parent_log);
	if (err)
		goto error;

	// Read parent metadata
	err = read_cbt_metadata(parent, fparent, &parent_log->metadata);
	if (err)
		goto error;

	// Open child log in r/w mode
	err = file_open(&fchild, child, "r+");
	if (err)
		goto error;

	err = malloc_cbt_log_data(&child_log);
	if (err)
		goto error;

	// Read child metadata
	err = read_cbt_metadata(child, fchild, &child_log->metadata);
	if (err)
		goto error;

	// check parent size is <= child size
	if (bitmap_size(parent_log->metadata.size) >
					bitmap_size(child_log->metadata.size)) {
		fprintf(stderr, "Parent bitmap larger than child bitmap,"
							"can't coalesce");
		err = -EINVAL;
		goto error;
	}

	//allocate and read cbt bitmap for parent
	err = allocate_cbt_bitmap(parent_log);
	if (err)
		goto error;

	err = read_cbt_bitmap(fparent, parent_log);
	if (err)
		goto error;

	//allocate and read cbt bitmap for child
	err = allocate_cbt_bitmap(child_log);
	if (err)
		goto error;

	err = read_cbt_bitmap(fchild, child_log);
	if (err)
		goto error;

	// Coalesce up to size of parent bitmap
	size = bitmap_size(parent_log->metadata.size);
	pbuf = parent_log->bitmap;
	cbuf = child_log->bitmap;

	while(size--){
		*cbuf++ |= *pbuf++;
	}

	// Set file pointer to start of bitmap area
	ret = fseek(fchild, sizeof(struct cbt_log_metadata), SEEK_SET);

	if(ret < 0) {
		fprintf(stderr, "Failed to seek to start of file %s. %s\n",
													child, strerror(errno));
		err = -errno;
		goto error;
	}

	size = bitmap_size(child_log->metadata.size);
	ret = fwrite(child_log->bitmap, size, 1, fchild);
	if (!ret) {
		fprintf(stderr, "Failed to write bitmap to log file %s\n", child);
		err = -EIO;
		goto error;
	}

error:
	if (parent_log) {
		if (parent_log->bitmap)
			free(parent_log->bitmap);
		free(parent_log);
	}
	if (child_log) {
		if (child_log->bitmap)
			free(child_log->bitmap);
		free(child_log);
	}

	if (fparent)
		fclose(fparent);
	if (fchild)
		fclose(fchild);

	return err;

usage:
	printf("cbt-util coalesce: Coalesce contents of parent bitmap on to child bitmap\n\n");
	printf("Options:\n\t-p Parent log file name\n");
	printf("\t-c Child log file name\n");
	printf("\t[-h help]\n");

	return -EINVAL;

}

void
help(void)
{
	printf("usage: cbt-util COMMAND [OPTIONS]\n");
	print_commands();
}

struct command *
get_command(char *command)
{
	int i, n;

	if (strnlen(command, 25) >= 25)
		return NULL;

	n = sizeof(commands) / sizeof (struct command);

	for (i = 0; i < n; i++)
		if (!strcmp(command, commands[i].name))
			return &commands[i];

	return NULL;
}
