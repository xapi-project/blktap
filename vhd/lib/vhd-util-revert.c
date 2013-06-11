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
