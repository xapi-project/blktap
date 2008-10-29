/* Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 *
 * Altering operations:
 *
 * 1. Change the parent pointer to another file.
 * 2. Change the size of the file containing the VHD image. This does NOT 
 * affect the VHD disk capacity, only the physical size of the file containing 
 * the VHD. Naturally, it is not possible to set the file size to be less than  
 * the what VHD utilizes.
 * The operation doesn't actually change the file size, but it writes the 
 * footer in the right location such that resizing the file (manually, as a 
 * separate step) will produce the correct results. If the new file size is 
 * greater than the current file size, the file must first be expanded and then 
 * altered with this operation. If the new size is smaller than the current 
 * size, the VHD must first be altered with this operation and then the file 
 * must be shrunk. Failing to resize the file will result in a corrupted VHD.
 */

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
