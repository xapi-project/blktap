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
