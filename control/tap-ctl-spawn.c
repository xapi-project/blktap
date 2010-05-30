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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "tap-ctl.h"
#include "blktap2.h"

static void
usage(void)
{
	printf("usage: spawn <-i id>\n");
}

static pid_t
__tap_ctl_spawn(const int id)
{
	int err, child;
	char uuid[12], *control, *tapdisk;

	if ((child = fork()) == -1) {
		printf("fork failed: %d\n", errno);
		return -errno;
	}

	if (child)
		return child;

	err = asprintf(&control, "%s/%s%d",
		       BLKTAP2_CONTROL_DIR,
		       BLKTAP2_CONTROL_SOCKET, id);
	if (err == -1) {
		printf("fork failed: %d\n", ENOMEM);
		exit(ENOMEM);
	}

	tapdisk = getenv("TAPDISK2");
	if (!tapdisk)
		tapdisk = "tapdisk2";

	snprintf(uuid, sizeof(uuid) - 1, "%d", id);

	execlp(tapdisk, tapdisk, "-u", uuid, "-c", control, NULL);

	printf("exec failed\n");
	exit(1);
}

pid_t
_tap_ctl_get_pid(const int id)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_PID;

	err = tap_ctl_connect_send_and_receive(id, &message, 2);
	if (err)
		return err;

	return message.u.tapdisk_pid;
}

static int
_tap_ctl_wait(pid_t child)
{
	pid_t pid;
	int status;

	pid = waitpid(child, &status, 0);
	if (pid < 0) {
		fprintf(stderr, "wait(%d) failed, err %d\n", child, errno);
		return -errno;
	}

	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);
		if (code)
			fprintf(stderr, "tapdisk2[%d] failed, status %d\n", child, code);
		return -code;
	}

	if (WIFSIGNALED(status)) {
		int signo = WTERMSIG(status);
		fprintf(stderr, "tapdisk2[%d] killed by signal %d\n", child, signo);
		return -EINTR;
	}

	fprintf(stderr, "tapdisk2[%d]: unexpected status %#x\n", child, status);
	return -EAGAIN;
}


int
_tap_ctl_spawn(const int id)
{
	pid_t child, task;
	int err;

	child = __tap_ctl_spawn(id);
	if (child < 0)
		return child;

	err = _tap_ctl_wait(child);
	if (err)
		return err;

	task = _tap_ctl_get_pid(id);
	if (task < 0)
		fprintf(stderr, "get_pid(%d) failed, err %d\n", child, errno);

	return task;
}

int
tap_ctl_spawn(int argc, char **argv)
{
	int c, id;
	pid_t task;

	id = -1;

	optind = 0;
	while ((c = getopt(argc, argv, "i:h")) != -1) {
		switch (c) {
		case 'i':
			id = atoi(optarg);
			break;
		case 'h':
			usage();
			return 0;
		}
	}

	if (id == -1) {
		usage();
		return EINVAL;
	}

	task = _tap_ctl_spawn(id);

	return task < 0 ? task : 0;
}
