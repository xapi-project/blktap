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
#include <unistd.h>
#include <libgen.h>
#include <getopt.h>

#include "tapdisk-utils.h"
#include "tapdisk-server.h"

static const char *program;

static void
usage(FILE *stream)
{
	fprintf(stream, "blktap-utils: v2.0.0\n");
	fprintf(stream, "Usage: %s [-h ] [-D] <READ fifo> <WRITE fifo>\n", program);
}

int
main(int argc, char *argv[])
{
	const struct option longopts[] = {
		{ "help",		0, 0, 'h' },
		{ "detach",		0, 0, 'D' }
	};
	int err, detach = 0;
	const char *facility;
	const char *ipc_read, *ipc_write;

	program  = basename(argv[0]);
	facility = "daemon";

	do {
		int c;

		c = getopt_long(argc, argv, "hDl:", longopts, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'l':
			facility = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		case 'D':
			detach = 1;
			break;
		default:
			goto usage;
		}
	} while (1);
	
	if (argc - optind < 2)
		goto usage;

	ipc_read  = argv[optind++];
	ipc_write = argv[optind++];

	if (argc - optind)
		goto usage;

	if (detach) {
		/* NB. This is expected to rarely, if ever, be
		   used. Blktapctrl is already detached, and breaking
		   affiliation will be exactly not desirable. */
		err = daemon(0, 0);
		if (err) {
			PERROR("daemon");
			return -errno;
		}
	}

	tapdisk_start_logging("TAPDISK", facility);

	err = tapdisk_server_initialize(ipc_read, ipc_write);
	if (err) {
		EPRINTF("failed to initialize tapdisk server: %d\n", err);
		goto out;
	}

	err = tapdisk_server_run();

out:
	tapdisk_stop_logging();

	return err ? 1 : 0;

usage:
	usage(stderr);
	return 1;
}
