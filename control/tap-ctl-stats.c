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
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "tap-ctl.h"

#define ASSERT(p)                                      \
    do {                                               \
        if (!(p)) {                                    \
            EPRINTF("Assertion '%s' failed, line %d, " \
                "file %s", #p, __LINE__, __FILE__);    \
            *(int*)0 = 0;                              \
        }                                              \
    } while (0)

int
_tap_ctl_stats_connect_and_send(pid_t pid, const char *uuid)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
	tapdisk_message_t message;
	int sfd, err;

	memset(&message, 0, sizeof(message));
	message.type = TAPDISK_MESSAGE_STATS;

	if (uuid) {
		if (uuid[0] == '\0' || strnlen(uuid, TAPDISK_MAX_VBD_UUID_LENGTH)
				>= TAPDISK_MAX_VBD_UUID_LENGTH) {
			return ENAMETOOLONG;
		}
		strcpy(message.u.params.uuid, uuid);
	}

	err = tap_ctl_connect_id(pid, &sfd);
	if (err)
		return err;

	err = tap_ctl_write_message(sfd, &message, &timeout);
	if (err)
		return err;

	return sfd;
}

ssize_t
tap_ctl_stats(pid_t pid, char *buf, size_t size, const char *uuid)
{
	tapdisk_message_t message;
	int sfd, err;
	size_t len;

	ASSERT(uuid);

	sfd = _tap_ctl_stats_connect_and_send(pid, uuid);
	if (sfd < 0)
		return sfd;

	err = tap_ctl_read_message(sfd, &message, NULL);
	if (err)
		return err;

	len = message.u.info.length;
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
tap_ctl_stats_fwrite(pid_t pid, FILE *stream, const char *uuid)
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

	sfd = _tap_ctl_stats_connect_and_send(pid, uuid);
	if (sfd < 0) {
		err = sfd;
		goto out;
	}

	err = tap_ctl_read_message(sfd, &message, NULL);
	if (err)
		goto out;

	len = message.u.info.length;
	if (len < 0) {
		err = len;
		goto out;
	}

	while (len) {
		fd_set rfds;
		size_t in, out;
		int n;

		FD_ZERO(&rfds);
		FD_SET(sfd, &rfds);

		n = select(sfd + 1, &rfds, NULL, NULL, NULL);
		if (n < 0) {
			err = n;
			goto out;
		}

		in = read(sfd, buf, bufsz);
		if (in <= 0) {
			err = in;
			goto out;
		}

		len -= in;

		out = fwrite(buf, in, 1, stream);
		if (out != 1) {
			err = -errno;
			goto out;
		}
	}
	len = fwrite("\n", 1, 1, stream);

out:
	if (sfd >= 0)
		close(sfd);
	if (buf != MAP_FAILED)
		munmap(buf, bufsz);

	return err;
}
