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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>

#include "tap-ctl.h"
#include "blktap2.h"

static pid_t
__tap_ctl_spawn(int *readfd, const bool nodaemon)
{
	int child, channel[2];
	char *tapdisk;
	char *argv[3];

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

	if (nodaemon) {
		argv[1] = "-D";
		argv[2] = NULL;
	} else
		argv[1] = NULL;

	tapdisk = getenv("TAPDISK");
	if (!tapdisk)
		tapdisk = getenv("TAPDISK2");

	if (tapdisk) {
		argv[0] = tapdisk;
		execvp(tapdisk, argv);
		exit(errno);
	}

	argv[0] = TAPDISK_EXEC;
	execv(TAPDISK_EXECDIR "/" TAPDISK_EXEC, argv);

	if (errno == ENOENT)
		execv(TAPDISK_BUILDDIR "/" TAPDISK_EXEC, argv);

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
	int err;

	f = fdopen(readfd, "r");
	if (!f) {
		EPRINTF("fdopen failed: %d\n", errno);
		return -1;
	}

	errno = 0;
	err = fscanf(f, BLKTAP2_CONTROL_DIR"/"BLKTAP2_CONTROL_SOCKET"%d", &id);
	if (err != 1) {
		if (!errno) {
			EPRINTF("parsing id failed: read %d items instead of one\n", err);
			err = EINVAL;
		} else {
			err = errno;
			EPRINTF("parsing id failed: %s\n", strerror(err));
		}
		id = -err;
	}

	fclose(f);
	return id;
}

int
tap_ctl_spawn(const bool nodaemon)
{
	pid_t child;
	int err, id, readfd;

	readfd = -1;

again:
	child = __tap_ctl_spawn(&readfd, nodaemon);
	if (child < 0)
		return child;

	if (!nodaemon) {
		err = tap_ctl_wait(child);
		if (err) {
			if (err == -EAGAIN)
				goto again;
			return err;
		}

		id = tap_ctl_get_child_id(readfd);
		if (id < 0)
			EPRINTF("failed to get child ID for child %d: %s\n", child,
					strerror(-id));
	} else
		id = child;

	return id;
}
