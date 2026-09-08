/*
 * Copyright (c) 2017, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cbt-util-priv.h"

int
main(int argc, char *argv[])
{
	char **cargv;
	struct command *cmd;
	int cargc, i, cnt, ret;

	ret = 0;

	if (argc < 2) {
		help();
		exit(EINVAL);
	}

	cargc = argc - 1;
	cmd   = get_command(argv[1]);
	if (!cmd) {
		fprintf(stderr, "Invalid COMMAND %s\n", argv[1]);
		help();
		exit(0);
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
