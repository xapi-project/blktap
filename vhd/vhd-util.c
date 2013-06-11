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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libvhd.h"
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
	{ .name = "create",      .func = vhd_util_create        },
	{ .name = "snapshot",    .func = vhd_util_snapshot      },
	{ .name = "query",       .func = vhd_util_query         },
	{ .name = "read",        .func = vhd_util_read          },
	{ .name = "set",         .func = vhd_util_set_field     },
	{ .name = "repair",      .func = vhd_util_repair        },
	{ .name = "resize",      .func = vhd_util_resize        },
	{ .name = "fill",        .func = vhd_util_fill          },
	{ .name = "coalesce",    .func = vhd_util_coalesce      },
	{ .name = "modify",      .func = vhd_util_modify        },
	{ .name = "scan",        .func = vhd_util_scan          },
	{ .name = "check",       .func = vhd_util_check         },
	{ .name = "revert",      .func = vhd_util_revert        },
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

TEST_FAIL_EXTERN_VARS;

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
			libvhd_set_log_level(1);
			continue;
		}

		cargv[cnt++] = arg;
	}

#ifdef ENABLE_FAILURE_TESTING
	for (i = 0; i < NUM_FAIL_TESTS; i++) {
		TEST_FAIL[i] = 0;
		if (getenv(ENV_VAR_FAIL[i]))
			TEST_FAIL[i] = 1;
	}
#endif // ENABLE_FAILURE_TESTING

	ret = cmd->func(cnt, cargv);

	free(cargv);

	return (ret >= 0 ? ret : -ret);
}
