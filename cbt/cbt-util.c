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

#if 1
#define DFPRINTF(_f, _a...) fprintf(stdout, _f , ##_a)
#else
#define DFPRINTF(_f, _a...) ((void)0)
#endif

typedef int (*cbt_util_func_t) (int, char **);
int cbt_util_create(int , char **);
int cbt_util_set(int , char **);

struct command {
	char               *name;
	cbt_util_func_t     func;
};

struct command commands[] = {
	{ .name = "create",      .func = cbt_util_create        },
	{ .name = "set",         .func = cbt_util_set        },
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
cbt_util_set(int argc, char **argv)
{
	char *name, *parent, *child;
	int err, c, consistent, flag = 0, read; 
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

	if (!name) 
		goto usage;

	struct cbt_log_metadata *log_meta = malloc(sizeof(struct cbt_log_metadata));
	if (!log_meta) {
		err = -ENOMEM;
		goto error;
	}

	f = fopen(name, "r+");
	if (f == NULL) {
		fprintf(stderr, "%s: failed to open log file: %d\n", name, -errno);
		err = -errno;
		goto error;
	}
	
	read = fread(log_meta, sizeof(struct cbt_log_metadata), 1, f);

	if (read == 0) {
		err = -EINVAL;
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
	fseek(f, 0, SEEK_SET);
	fwrite(log_meta, sizeof(struct cbt_log_metadata), 1, f);

error:
	if(log_meta)
		free(log_meta);
	if(f)
		fclose(f);
	return err;

usage:
	printf("cbt-util set: Set field in log file\n\n");
	printf("Options:\n\t-n Log file name\n\t[-p Parent log file UUID]\n"
			"\t[-c Child log file UUID]\n\t[-f 0|1 Consistency flag]\n"
			"\t[-h help]\n");

	return -EINVAL;
}

int 
cbt_util_create(int argc, char **argv)
{
	char *name;
	int err, c;
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

	printf("Name parsed as: %s, size parsed as: %lu\n", name, size);

	if (!name || !size) 
		goto usage;

	fprintf(stderr, "Initialising metadata for file %s\n", name);

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
		fprintf(stderr, "%s: failed to create: %d\n", name, -errno);
		err = -errno;
		goto error;
	}
	
	fwrite(&log_data->metadata, sizeof(struct cbt_log_metadata), 1, f);
	fwrite(log_data->bitmap, bitmap_sz, 1, f);

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

int
main(int argc, char *argv[])
{
	char **cargv;
	struct command *cmd;
	int cargc, i, cnt, ret;

	ret = 0;

	if (argc < 2)
		help();

	cargc = argc - 1;
	cmd   = get_command(argv[1]);
	if (!cmd) {
		fprintf(stderr, "invalid COMMAND %s\n", argv[1]);
		help();
	}

	cargv = malloc(sizeof(char *) * cargc);
	if (!cargv)
		exit(ENOMEM);

	cnt      = 1;
	cargv[0] = cmd->name;
	for (i = 1; i < cargc; i++) {
		char *arg = argv[i + (argc - cargc)];

		cargv[cnt++] = arg;
	}

	ret = cmd->func(cnt, cargv);

	free(cargv);

	return (ret >= 0 ? ret : -ret);
}
