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
