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

#include "cbt-util.h"
#include "cbt-util-priv.h"

int cbt_util_create(int , char **);
int cbt_util_set(int , char **);
int cbt_util_get(int , char **);



struct command commands[] = {
	{ .name = "create", .func = cbt_util_create},
	{ .name = "set", .func = cbt_util_set},
	{ .name = "get", .func = cbt_util_get},
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
cbt_util_get(int argc, char **argv)
{
	char *name, uuid_str[37];
	int err, c, ret; 
	int parent, child, flag; 
	FILE *f = NULL;

	err			= 0;
	name		= NULL;
	parent		= 0;
	child		= 0;
	flag		= 0;

	if (!argc || !argv)
		goto usage;

	/* Make sure we start from the start of the args */
	optind = 1;

	while ((c = getopt(argc, argv, "n:pcfh")) != -1) {
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
			case 'h':
			default:
				goto usage;
		}
	}

	// Exactly one of p, c or f must be queried for
	if (!name || (parent + child + flag != 1)) 
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
	}

error:
	if(log_meta)
		free(log_meta);
	if(f)
		fclose(f);
	return err;

usage:
	printf("cbt-util get: Read field from log file\n\n");
	printf("Options:\n -n name\tName of log file\n[-p]\t\t"
			"Print parent log file UUID\n[-c]\t\tPrint child log file UUID\n"
			"[-f]\t\tPrint consistency flag\n[-h]\t\thelp\n");

	return -EINVAL;
}


int 
cbt_util_set(int argc, char **argv)
{
	char *name, *parent, *child;
	int err, c, consistent, flag = 0, ret; 
	FILE *f = NULL;

	err 	= 0;
	name 	= NULL;
	parent	= NULL;
	child	= NULL;

	if (!argc || !argv)
		goto usage;

	while ((c = getopt(argc, argv, "n:p:c:f:h")) != -1) {
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
			case 'h':
			default:
				goto usage;
		}
	}

	//TODO:Check at least one of p, c or f is supplied?
	if (!name) 
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

	if(flag) {
		log_meta->consistent = consistent;
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
	printf("Options:\n -n name\tName of log file\n[-p parent]\t"
			"Parent log file UUID\n[-c child]\tChild log file UUID\n[-f 0|1]"
			"\tConsistency flag\n[-h]\t\thelp\n");

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

void
help(void)
{
	printf("usage: cbt-util COMMAND [OPTIONS]\n");
	print_commands();
	exit(0);
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
