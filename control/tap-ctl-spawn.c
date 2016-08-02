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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>

#include "tap-ctl.h"
#include "blktap2.h"

static pid_t
__tap_ctl_spawn(int *readfd)
{
	int child, channel[2];
	char *tapdisk;

	if (pipe(channel)) {
		EPRINTF("pipe failed: %d\n", errno);
		return -errno;
	}

	if ((child = fork()) == -1) {
		EPRINTF("fork failed: %d\n", errno);
		return -errno;
	}

	if (child) {
		close(channel[1]);
		*readfd = channel[0];
		return child;
	}

	if (dup2(channel[1], STDOUT_FILENO) == -1) {
		EPRINTF("dup2 failed: %d\n", errno);
		exit(errno);
	}

	if (dup2(channel[1], STDERR_FILENO) == -1) {
		EPRINTF("dup2 failed: %d\n", errno);
		exit(errno);
	}

	close(channel[0]);
	close(channel[1]);

	tapdisk = getenv("TAPDISK");
	if (!tapdisk)
		tapdisk = getenv("TAPDISK2");

	if (tapdisk) {
		execlp(tapdisk, tapdisk, NULL);
		exit(errno);
	}

	execl(TAPDISK_EXECDIR "/" TAPDISK_EXEC, TAPDISK_EXEC,
	      NULL);

	if (errno == ENOENT)
		execl(TAPDISK_BUILDDIR "/" TAPDISK_EXEC, TAPDISK_EXEC,
		      NULL);

	exit(errno);
}

pid_t
tap_ctl_get_pid(const int id)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_PID;

	err = tap_ctl_connect_send_and_receive(id, &message, NULL);
	if (err)
		return err;

	return message.u.tapdisk_pid;
}

static int
tap_ctl_wait(pid_t child)
{
	pid_t pid;
	int status;

	pid = waitpid(child, &status, 0);
	if (pid < 0) {
		EPRINTF("wait(%d) failed, err %d\n", child, errno);
		return -errno;
	}

	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);
		if (code)
			EPRINTF("tapdisk2[%d] failed, status %d\n", child, code);
		return -code;
	}

	if (WIFSIGNALED(status)) {
		int signo = WTERMSIG(status);
		EPRINTF("tapdisk2[%d] killed by signal %d\n", child, signo);
		if (signo == SIGUSR1)
			/* NB. there's a race between tapdisk's
			 * sigaction init and xen-bugtool shooting
			 * debug signals. If killed by something as
			 * innocuous as USR1, then retry. */
			return -EAGAIN;
		return -EINTR;
	}

	EPRINTF("tapdisk2[%d]: unexpected status %#x\n", child, status);
	return -EAGAIN;
}

static int
tap_ctl_get_child_id(int readfd)
{
	int id;
	FILE *f;

	f = fdopen(readfd, "r");
	if (!f) {
		EPRINTF("fdopen failed: %d\n", errno);
		return -1;
	}

	errno = 0;
	if (fscanf(f, BLKTAP2_CONTROL_DIR"/"
		   BLKTAP2_CONTROL_SOCKET"%d", &id) != 1) {
		errno = (errno ? : EINVAL);
		EPRINTF("parsing id failed: %d\n", errno);
		id = -1;
	}

	fclose(f);
	return id;
}

int
tap_ctl_spawn(void)
{
	pid_t child;
	int err, id, readfd;

	readfd = -1;

again:
	child = __tap_ctl_spawn(&readfd);
	if (child < 0)
		return child;

	err = tap_ctl_wait(child);
	if (err) {
		if (err == -EAGAIN)
			goto again;
		return err;
	}

	id = tap_ctl_get_child_id(readfd);
	if (id < 0)
		EPRINTF("get_id failed, child %d err %d\n", child, errno);

	return id;
}
