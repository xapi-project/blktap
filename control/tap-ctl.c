/*
 * Copyright (c) 2008, XenSource Inc.
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
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "tap-ctl.h"

typedef int (*tap_ctl_func_t) (int, char **);

struct command {
	char                     *name;
	tap_ctl_func_t            func;
};

static int
tap_cli_get_driver_id(const char *handle)
{
	int type;

	type = tap_ctl_get_driver_id(handle);
	if (type < 0)
		fprintf(stderr, "No such driver '%s': %d\n", handle, type);

	return type;
}

static void
tap_cli_list_usage(FILE *stream)
{
	fprintf(stream,
		"usage: list [-h] [-m minor] [-i id] [-p pid] [-t type] [-f file]\n");
}

int
tap_cli_list(int argc, char **argv)
{
	tap_list_t **list, **_entry;
	int c, id, minor, type, err;
	const char *driver, *file;
	pid_t pid;

	err = tap_ctl_list(&list);
	if (err)
		return -err;

	id     = -1;
	minor  = -1;
	pid    = -1;
	type   = -1;
	driver = NULL;
	file   = NULL;

	while ((c = getopt(argc, argv, "m:i:p:t:f:h")) != -1) {
		switch (c) {
		case 'i':
			id = atoi(optarg);
			break;
		case 'm':
			minor = atoi(optarg);
			break;
		case 'p':
			pid = atoi(optarg);
			break;
		case 't':
			driver = optarg;
			type = tap_cli_get_driver_id(driver);
			if (type < 0)
				return -type;
			break;
		case 'f':
			file = optarg;
			break;
		case 'h':
			tap_cli_list_usage(stdout);
			return 0;
		}
	}

	for (_entry = list; *_entry != NULL; ++_entry) {
		tap_list_t *entry  = *_entry;
		char id_str[10]    = "-";
		char minor_str[10] = "-";
		char state_str[10] = "-";
		char pid_str[10]   = "-";

		if (id >= 0 && entry->id != id)
			continue;

		if (minor >= 0 && entry->minor != minor)
			continue;

		if (pid >= 0 && entry->pid != pid)
			continue;

		if (driver && entry->driver && strcmp(entry->driver, driver))
			continue;

		if (file && entry->path && strcmp(entry->path, file))
			continue;

		if (entry->id != -1)
			sprintf(id_str, "%d", entry->id);

		if (entry->pid != -1)
			sprintf(pid_str, "%d", entry->pid);

		if (entry->minor != -1)
			sprintf(minor_str, "%d", entry->minor);

		if (entry->state != -1)
			sprintf(state_str, "%x", entry->state);

		printf("%-3s %2s %8s %4s %10s %s\n",
		       minor_str, id_str, pid_str, state_str,
		       entry->driver ? : "-", entry->path ? : "-");
	}

	tap_ctl_free_list(list);

	return 0;

usage:
	tap_cli_list_usage(stderr);
	return EINVAL;
}

static void
tap_cli_pause_usage(FILE *stream)
{
	fprintf(stream, "usage: pause <-i id> <-m minor>\n");
}

static int
tap_cli_pause(int argc, char **argv)
{
	int c, id, minor;

	id    = -1;
	minor = -1;

	optind = 0;
	while ((c = getopt(argc, argv, "i:m:h")) != -1) {
		switch (c) {
		case 'i':
			id = atoi(optarg);
			break;
		case 'm':
			minor = atoi(optarg);
			break;
		case 'h':
			tap_cli_pause_usage(stdout);
			return 0;
		}
	}

	if (id == -1 || minor == -1) {
		tap_cli_pause_usage(stderr);
		return EINVAL;
	}

	return tap_ctl_pause(id, minor);
}

static void
tap_cli_unpause_usage(FILE *stream)
{
	fprintf(stream, "usage: unpause <-i id> <-m minor> <-t type> <-f file>\n");
}

int
tap_cli_unpause(int argc, char **argv)
{
	char *file;
	int c, id, minor, type;

	id    = -1;
	minor = -1;
	type  = -1;
	file  = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "i:m:t:f:h")) != -1) {
		switch (c) {
		case 'i':
			id = atoi(optarg);
			break;
		case 'm':
			minor = atoi(optarg);
			break;
		case 't':
			type = atoi(optarg);
			break;
		case 'f':
			file = optarg;
			break;
		case 'h':
			tap_cli_unpause_usage(stdout);
			return 0;
		}
	}

	if (id == -1 || minor == -1) {
		tap_cli_unpause_usage(stderr);
		return EINVAL;
	}

	return tap_ctl_unpause(id, minor, type, file);
}

static void
tap_cli_major_usage(FILE *stream)
{
	fprintf(stream, "usage: unpause <-i id> <-m minor> <-t type> <-f file>\n");
}

static int
tap_cli_major(int argc, char **argv)
{
	int c, chr, major;

	chr = 0;

	while ((c = getopt(argc, argv, "bch")) != -1) {
		switch (c) {
		case 'b':
			chr = 0;
			break;
		case 'c':
			chr = 1;
			break;
		case 'h':
			tap_cli_major_usage(stdout);
			return 0;
		default:
			goto usage;
		}
	}

	if (chr)
		major = -EINVAL;
	else
		major = tap_ctl_blk_major();

	if (major < 0)
		return -major;

	printf("%d\n", major);

	return 0;

usage:
	tap_cli_major_usage(stderr);
	return EINVAL;
}

static void
tap_cli_open_usage(FILE *stream)
{
	fprintf(stream, "usage: open <-t type> <-f file> <-i id> <-m minor>\n");
}

static int
tap_cli_open(int argc, char **argv)
{
	const char *file;
	int c, id, minor, type;

	id    = -1;
	type  = -1;
	minor = -1;
	file  = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "i:m:t:f:h")) != -1) {
		switch (c) {
		case 'i':
			id = atoi(optarg);
			break;
		case 'm':
			minor = atoi(optarg);
			break;
		case 't':
			type = tap_cli_get_driver_id(optarg);
			if (type < 0)
				return -type;
			break;
		case 'f':
			file = optarg;
			break;
		case 'h':
			tap_cli_open_usage(stdout);
			return 0;
		}
	}

	if (id == -1 || minor == -1 || type == -1 || !file)
		goto usage;

	return tap_ctl_open(id, minor, type, file);

usage:
	tap_cli_open_usage(stderr);
	return EINVAL;
}

struct command commands[] = {
	{ .name = "list",         .func = tap_cli_list          },
	{ .name = "allocate",     .func = tap_ctl_allocate      },
	{ .name = "free",         .func = tap_ctl_free          },
	{ .name = "create",       .func = tap_ctl_create        },
	{ .name = "destroy",      .func = tap_ctl_destroy       },
	{ .name = "spawn",        .func = tap_ctl_spawn         },
	{ .name = "attach",       .func = tap_ctl_attach        },
	{ .name = "detach",       .func = tap_ctl_detach        },
	{ .name = "open",         .func = tap_cli_open          },
	{ .name = "close",        .func = tap_ctl_close         },
	{ .name = "pause",        .func = tap_cli_pause         },
	{ .name = "unpause",      .func = tap_cli_unpause       },
	{ .name = "major",        .func = tap_cli_major         },
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

void
help(void)
{
	printf("usage: tap-ctl COMMAND [OPTIONS]\n");
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

#ifdef CORE_DUMP
	#include <sys/resource.h>
	struct rlimit rlim;
	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;
	if (setrlimit(RLIMIT_CORE, &rlim) < 0)
		fprintf(stderr, "setrlimit failed: %d\n", errno);
#endif

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

		if (!strcmp(arg, "--debug")) {
			tap_ctl_debug = 1;
			continue;
		}

		cargv[cnt++] = arg;
	}

	ret = cmd->func(cnt, cargv);

	free(cargv);

	return (ret >= 0 ? ret : -ret);
}
