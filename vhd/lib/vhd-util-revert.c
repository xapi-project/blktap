/*
 * Copyright (c) 2008, XenSource Inc.
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
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
