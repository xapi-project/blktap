/* Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vhd-util.h"

#if 1
#define DFPRINTF(_f, _a...) fprintf(stdout, _f , ##_a)
#else
#define DFPRINTF(_f, _a...) ((void)0)
#endif

typedef int (*vhd_util_func_t) (int, char **);

struct command {
	char               *name;
	vhd_util_func_t     func;
};

struct command commands[] = {
	{ .name = "resize",      .func = vhd_util_resize  },
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
	printf("usage: vhd-util COMMAND [OPTIONS]\n");
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
	int cargc, i, ret;
	struct command *cmd;

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

	cargv[0] = cmd->name;
	for (i = 1; i < cargc; i++)
		cargv[i] = argv[i + (argc - cargc)];

	ret = cmd->func(cargc, cargv);

	free(cargv);

	return (ret >= 0 ? ret : -ret);
}
