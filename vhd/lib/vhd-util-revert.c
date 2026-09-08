/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
//#include <fcntl.h>
#include <stdio.h>
//#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"
#include "libvhd-journal.h"

int
vhd_util_revert(int argc, char **argv)
{
	char *name, *jname;
	vhd_journal_t journal;
	int c, err;

	name  = NULL;
	jname = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "n:j:h")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'j':
			jname = optarg;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || !jname || argc != optind)
		goto usage;

	libvhd_set_log_level(1);
	err = vhd_journal_open(&journal, name, jname);
	if (err) {
		printf("opening journal failed: %d\n", err);
		return err;
	}

	err = vhd_journal_revert(&journal);
	if (err) {
		printf("reverting journal failed: %d\n", err);
		vhd_journal_close(&journal);
		return err;
	}

	err = vhd_journal_remove(&journal);
	if (err) {
		printf("removing journal failed: %d\n", err);
		vhd_journal_close(&journal);
		return err;
	}

	return 0;

usage:
	printf("options: <-n name> <-j journal> [-h help]\n");
	return -EINVAL;
}
