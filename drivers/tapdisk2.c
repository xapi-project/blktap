/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

	DPRINTF("Tapdisk running, control on %s\n", control);

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
	if (err) {
		EPRINTF("Tapdisk exiting with error %d\n", err);
	}
	td_metrics_stop();
	tdnbd_fdreceiver_stop();
	tapdisk_control_close();
	tapdisk_stop_logging();
	return -err;
}
