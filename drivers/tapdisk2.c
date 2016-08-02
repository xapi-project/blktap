/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "tapdisk.h"
#include "tapdisk-utils.h"
#include "tapdisk-server.h"
#include "tapdisk-control.h"
#include "tapdisk-metrics.h"

void tdnbd_fdreceiver_start();
void tdnbd_fdreceiver_stop();

static void
usage(const char *app, int err)
{
	fprintf(stderr, "usage: %s <-u uuid> <-c control socket>\n", app);
	exit(err);
}

static FILE *
fdup(FILE *stream, const char *mode)
{
	int fd, err;
	FILE *f;

	fd = dup(STDOUT_FILENO);
	if (fd < 0)
		goto fail;

	f = fdopen(fd, mode);
	if (!f)
		goto fail;

	return f;

fail:
	err = -errno;
	if (fd >= 0)
		close(fd);
	errno = -err;

	return NULL;
}

int
main(int argc, char *argv[])
{
	char *control;
	int c, err, nodaemon;
	FILE *out;

	control  = NULL;
	nodaemon = 0;

	while ((c = getopt(argc, argv, "Dh")) != -1) {
		switch (c) {
		case 'D':
			nodaemon = 1;
			break;
		case 'h':
			usage(argv[0], 0);
			break;
		default:
			usage(argv[0], EINVAL);
		}
	}

	if (optind != argc)
		usage(argv[0], EINVAL);

	err = tapdisk_server_init();
	if (err) {
		DPRINTF("failed to initialize server: %d\n", err);
		goto out;
	}

	out = fdup(stdout, "w");
	if (!out) {
		err = -errno;
		DPRINTF("failed to dup stdout: %d\n", err);
		goto out;
	}

	if (!nodaemon) {
		err = daemon(0, 0);
		if (err) {
			DPRINTF("failed to daemonize: %d\n", errno);
			goto out;
		}
	}

	tapdisk_start_logging("tapdisk", NULL);

	err = tapdisk_control_open(&control);
	if (err) {
		DPRINTF("failed to open control socket: %d\n", err);
		goto out;
	}

	err = tapdisk_server_complete();
	if (err) {
		DPRINTF("failed to complete server: %d\n", err);
		goto out;
	}

	fprintf(out, "%s\n", control);
	fclose(out);

	err = td_metrics_start();
	if (err) {
		DPRINTF("failed to create metrics folder: %d\n", err);
		goto out;
	}
	/*
	 * NB: We're unconditionally starting the FD receiver here - this is 
	 * for the block-nbd driver. In the future we may want to start this as 
	 * a response to a tap-ctl message
	 */
	tdnbd_fdreceiver_start();

	err = tapdisk_server_run();

out:
	td_metrics_stop();
	tdnbd_fdreceiver_stop();
	tapdisk_control_close();
	tapdisk_stop_logging();
	return -err;
}
