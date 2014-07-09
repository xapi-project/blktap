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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "tap-ctl.h"
#include "blktap2.h"
#include "compiler.h"

int tap_ctl_debug = 0;

int
tap_ctl_read_raw(int fd, void *buf, size_t size, struct timeval *timeout)
{
	fd_set readfds;
	size_t offset = 0;
	int ret;

	while (offset < size) {
		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);

		ret = select(fd + 1, &readfds, NULL, NULL, timeout);
		if (ret == -1)
			break;
		else if (FD_ISSET(fd, &readfds)) {
			ret = read(fd, buf + offset, size - offset);
			if (ret <= 0)
				break;
			offset += ret;
		} else
			break;
	}

	if (offset != size) {
		EPRINTF("failure reading data %zd/%zd\n", offset, size);
		return -EIO;
	}

	return 0;
}

int
tap_ctl_read_message(int fd, tapdisk_message_t *message,
		     struct timeval *timeout)
{
	size_t size = sizeof(tapdisk_message_t);
	int err;

	err = tap_ctl_read_raw(fd, message, size, timeout);
	if (err)
		return err;

	DBG("received '%s' message (uuid = %u)\n",
	    tapdisk_message_name(message->type), message->cookie);

	return 0;
}

int
tap_ctl_write_message(int fd, tapdisk_message_t *message, struct timeval *timeout)
{
	fd_set writefds;
	int ret, len, offset;

	offset = 0;
	len    = sizeof(tapdisk_message_t);

	DBG("sending '%s' message (uuid = %u)\n",
	    tapdisk_message_name(message->type), message->cookie);

	while (offset < len) {
		FD_ZERO(&writefds);
		FD_SET(fd, &writefds);

		/* we don't bother reinitializing tv. at worst, it will wait a
		 * bit more time than expected. */

		ret = select(fd + 1, NULL, &writefds, NULL, timeout);
		if (ret == -1)
			break;
		else if (FD_ISSET(fd, &writefds)) {
			ret = write(fd, message + offset, len - offset);
			if (ret <= 0)
				break;
			offset += ret;
		} else
			break;
	}

	if (offset != len) {
		EPRINTF("failure writing message\n");
		return -EIO;
	}

	return 0;
}

int
tap_ctl_send_and_receive(int sfd, tapdisk_message_t *message,
			 struct timeval *timeout)
{
	int err;

	err = tap_ctl_write_message(sfd, message, timeout);
	if (err) {
		EPRINTF("failed to send '%s' message\n",
			tapdisk_message_name(message->type));
		return err;
	}

	err = tap_ctl_read_message(sfd, message, timeout);
	if (err) {
		EPRINTF("failed to receive '%s' message\n",
			tapdisk_message_name(message->type));
		return err;
	}

	return 0;
}

char *
tap_ctl_socket_name(int id)
{
	char *name;

	if (asprintf(&name, "%s/%s%d",
		     BLKTAP2_CONTROL_DIR, BLKTAP2_CONTROL_SOCKET, id) == -1)
		return NULL;

	return name;
}

int
tap_ctl_connect(const char *name, int *sfd)
{
	int fd, err;
	struct sockaddr_un saddr;

	*sfd = -1;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		EPRINTF("couldn't create socket for %s: %s\n", name, strerror(errno));
		return -errno;
	}

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, name);

	err = connect(fd, (const struct sockaddr *)&saddr, sizeof(saddr));
	if (err) {
		err = errno;
		if (likely(err == ENOENT))
			DPRINTF("couldn't connect to %s: %s\n", name, strerror(err));
		else
			EPRINTF("couldn't connect to %s: %s\n", name, strerror(err));
		close(fd);
		return -err;
	}

	*sfd = fd;
	return 0;
}

int
tap_ctl_connect_id(int id, int *sfd)
{
	int err;
	char *name;

	*sfd = -1;

	if (id < 0) {
		EPRINTF("invalid id %d\n", id);
		return -EINVAL;
	}

	name = tap_ctl_socket_name(id);
	if (!name) {
		EPRINTF("couldn't name socket for %d\n", id);
		return -ENOMEM;
	}

	err = tap_ctl_connect(name, sfd);

	free(name);

	return err;
}

int
tap_ctl_connect_send_and_receive(int id, tapdisk_message_t *message,
				 struct timeval *timeout)
{
	int err, sfd;

	err = tap_ctl_connect_id(id, &sfd);
	if (err)
		return err;

	err = tap_ctl_send_and_receive(sfd, message, timeout);

	close(sfd);
	return err;
}
