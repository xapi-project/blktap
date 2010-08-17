/*
 * Copyright (c) 2010, Citrix 
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
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "tap-ctl.h"

int
_tap_ctl_stats_connect_and_send(pid_t pid, int minor)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
	tapdisk_message_t message;
	int sfd, err;
	size_t len;

	err = tap_ctl_connect_id(pid, &sfd);
	if (err)
		return err;

	memset(&message, 0, sizeof(message));
	message.type   = TAPDISK_MESSAGE_STATS;
	message.cookie = minor;

	err = tap_ctl_write_message(sfd, &message, &timeout);
	if (err)
		return err;

	return sfd;
}

ssize_t
tap_ctl_stats(pid_t pid, int minor, char *buf, size_t size)
{
	tapdisk_message_t message;
	int sfd, err;
	size_t len;

	sfd = _tap_ctl_stats_connect_and_send(pid, minor);
	if (sfd < 0)
		return sfd;

	err = tap_ctl_read_message(sfd, &message, NULL);
	if (err)
		return err;

	len= message.u.info.length;
	if (len < 0) {
		err = len;
		goto out;
	}
	if (size < len + 1)
		len = size - 1;

	err = tap_ctl_read_raw(sfd, buf, len, NULL);
	if (err)
		goto out;

	buf[len] = 0;

out:
	close(sfd);
	return err;
}

int
tap_ctl_stats_fwrite(pid_t pid, int minor, FILE *stream)
{
	tapdisk_message_t message;
	int sfd = -1, prot, flags, err;
	size_t len, bufsz;
	char *buf = MAP_FAILED;

	prot  = PROT_READ|PROT_WRITE;
	flags = MAP_ANONYMOUS|MAP_PRIVATE;
	bufsz = sysconf(_SC_PAGE_SIZE);

	buf = mmap(NULL, bufsz, prot, flags, -1, 0);
	if (buf == MAP_FAILED) {
		buf = NULL;
		err = -ENOMEM;
		goto out;
	}

	sfd = _tap_ctl_stats_connect_and_send(pid, minor);
	if (sfd < 0) {
		err = sfd;
		goto out;
	}

	err = tap_ctl_read_message(sfd, &message, NULL);
	if (err)
		goto out;

	len = message.u.info.length;
	err = len;
	if (len < 0)
		goto out;

	while (len) {
		fd_set rfds;
		size_t in, out;
		int n;

		FD_ZERO(&rfds);
		FD_SET(sfd, &rfds);

		n = select(sfd + 1, &rfds, NULL, NULL, NULL);
		err = n;
		if (n < 0)
			goto out;

		in = read(sfd, buf, bufsz);
		err = in;
		if (in <= 0)
			goto out;

		len -= in;

		out = fwrite(buf, in, 1, stream);
		if (out != in) {
			err = -errno;
			goto out;
		}
	}

out:
	if (sfd >= 0)
		close(sfd);
	if (buf != MAP_FAILED)
		munmap(buf, bufsz);

	return err;
}
