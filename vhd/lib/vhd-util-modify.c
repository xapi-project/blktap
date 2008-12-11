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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"
#include "libvhd-journal.h"

TEST_FAIL_EXTERN_VARS;

int
vhd_util_modify(int argc, char **argv)
{
	char *name, *jname;
	vhd_context_t vhd;
	vhd_journal_t journal;
	int err, jerr, c, size, parent, parent_raw;
	off64_t newsize = 0;
	char *newparent = NULL;

	name       = NULL;
	jname      = NULL;
	size       = 0;
	parent     = 0;
	parent_raw = 0;

	optind = 0;
	while ((c = getopt(argc, argv, "n:j:s:p:mh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'j':
			jname = optarg;
			break;
		case 's':
			size = 1;
			errno = 0;
			newsize = strtoll(optarg, NULL, 10);
			if (errno) {
				fprintf(stderr, "Invalid size '%s'\n", optarg);
				goto usage;
			}
			break;
		case 'p':
			parent = 1;
			newparent = optarg;
			break;
		case 'm':
			parent_raw = 1;
			break;

		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || !jname || optind != argc)
		goto usage;

	if (size) {
		err = vhd_open(&vhd, name, VHD_OPEN_RDWR);
		if (err) {
			printf("error opening %s: %d\n", name, err);
			return err;
		}

		err = vhd_set_phys_size(&vhd, newsize);
		if (err)
			printf("failed to set physical size to %"PRIu64":"
			       " %d\n", newsize, err);
		return err;
	}

	/* all journaled operations are below */

	libvhd_set_log_level(1);
	err = vhd_journal_create(&journal, name, jname);
	if (err) {
		printf("creating journal failed: %d\n", err);
		return err;
	}

	err = vhd_get_footer(&journal.vhd);
	if (err)
		goto out;

	if (parent) {
		TEST_FAIL_AT(FAIL_REPARENT_BEGIN);
		err = vhd_change_parent(&journal.vhd, newparent, parent_raw);
		if (err) {
			printf("failed to set parent to '%s': %d\n",
					newparent, err);
			goto out;
		}
		TEST_FAIL_AT(FAIL_REPARENT_END);
	}
		
out:
	if (err) {
		printf("modify failed: %d\n", err);
		jerr = vhd_journal_revert(&journal);
	} else
		jerr = vhd_journal_commit(&journal);

	if (jerr) {
		printf("closing journal failed: %d\n", jerr);
		vhd_journal_close(&journal);
	} else
		vhd_journal_remove(&journal);

	return (err ? : jerr);

usage:
	printf("*** Dangerous operations, use with care ***\n");
	printf("options: <-n name> <-j journal> [-p NEW_PARENT set parent "
			"[-m raw]] [-s NEW_SIZE set size] [-h help]\n");
	return -EINVAL;
}
