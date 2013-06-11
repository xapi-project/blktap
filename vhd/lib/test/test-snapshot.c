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

/*
 * libvhdio.so supports a simple test hook for validating vhd chains:
 * if LIBVHD_IO_TEST is set, libvhdio will handle SIGCONT specially
 * by closing, snapshotting, and reopening any vhds it is tracking.
 *
 * this harness simply forks a test and stops/continues it at a given interval.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static void
usage(const char *app, int err)
{
	printf("usage: %s <-i interval> -- <app and args>\n", app);
	exit(err);
}

static void
sighandler(int sig)
{
	fprintf(stderr, "child exited\n");
	exit(0);
}

static void
stop(pid_t pid)
{
	int status;

	fprintf(stderr, "stopping %d\n", pid);

	if (kill(pid, SIGSTOP)) {
		perror("stop child");
		exit(1);
	}

	if (waitpid(pid, &status, WUNTRACED) == -1) {
		perror("waiting for child to stop");
		exit(1);
	}

	if (WIFEXITED(status))
		exit(0);

	if (!WIFSTOPPED(status)) {
		perror("child not stopped");
		exit(1);
	}
}

static void
resume(pid_t pid)
{
	int status;

	fprintf(stderr, "resuming %d\n", pid);

	if (kill(pid, SIGCONT)) {
		perror("resume child");
		exit(1);
	}

	if (waitpid(pid, &status, WCONTINUED) == -1) {
		perror("waiting for child to resume");
		exit(1);
	}

	if (WIFEXITED(status))
		exit(0);

	if (!WIFCONTINUED(status)) {
		perror("child not resumed");
		exit(1);
	}
}

static void
test(pid_t pid, int interval)
{
	for (;;) {
		fprintf(stderr, "sleeping\n");
		sleep(interval);
		stop(pid);
		resume(pid);
	}
}

int
main(int argc, char **argv)
{
	pid_t pid;
	sigset_t set;
	int c, interval;
	struct sigaction act;

	interval = 0;

	while ((c = getopt(argc, argv, "i:h")) != -1) {
		switch (c) {
		case 'i':
			interval = atoi(optarg);
			break;
		case 'h':
			usage(argv[0], 0);
			break;
		default:
			usage(argv[0], EINVAL);
			break;
		}
	}

	if (optind == argc || !interval)
		usage(argv[0], EINVAL);

	if (sigemptyset(&set)) {
		perror("init sigset");
		exit(1);
	}

	act = (struct sigaction) {
		.sa_handler = sighandler,
		.sa_mask    = set,
		.sa_flags   = SA_NOCLDSTOP,
	};

	if (sigaction(SIGCHLD, &act, NULL)) {
		perror("register sig handler");
		exit(1);
	}

	switch ((pid = fork())) {
	case 0:
		if (putenv("LIBVHD_IO_TEST=y")) {
			perror("setting environment");
			exit(errno);
		}

		execvp(argv[optind], &argv[optind]);

		perror("exec");
		exit(errno);
	case -1:
		perror("fork");
		exit(errno);
	default:
		test(pid, interval);
		break;
	}

	return 0;
}
