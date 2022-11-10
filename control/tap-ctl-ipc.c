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
#include "util.h"

int tap_ctl_debug = 0;

#define eintr_retry(res, op) \
	do {		     \
		res = op;    \
	} while (res == -1 && errno == EINTR);

int
tap_ctl_read_raw(int fd, void *buf, size_t size, struct timeval *timeout)
{
	fd_set readfds;
	size_t offset = 0;
	int ret;

	while (offset < size) {
		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);

		eintr_retry(ret, select(fd + 1, &readfds, NULL, NULL, timeout))
		if (ret == -1)
			break;
		else if (FD_ISSET(fd, &readfds)) {
			eintr_retry(ret, read(fd, buf + offset, size - offset))
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

		eintr_retry(ret, select(fd + 1, NULL, &writefds, NULL, timeout))
		if (ret == -1)
			break;
		else if (FD_ISSET(fd, &writefds)) {
			eintr_retry(ret, write(fd, (uint8_t*)message + offset, len - offset))
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

int
tap_ctl_send_and_receive_ex(int sfd, tapdisk_message_t *message,
			    const char *logpath, const char *sockpath,
			    uint8_t key_size,
			    const uint8_t *encryption_key,
			    struct timeval *timeout)
{
	int err, ret;

	err = tap_ctl_write_message(sfd, message, timeout);
	if (err) {
		EPRINTF("failed to send '%s' message\n",
			tapdisk_message_name(message->type));
		return err;
	}

	if (message->u.params.flags & TAPDISK_MESSAGE_FLAG_ADD_LOG) {
		char buf[TAPDISK_MESSAGE_MAX_PATH_LENGTH];

		snprintf(buf, TAPDISK_MESSAGE_MAX_PATH_LENGTH - 1, "%s", logpath);

		ret = write(sfd, &buf, sizeof(buf));

		if (ret == -1) {
			EPRINTF("Failed to send logpath with '%s' message\n",
				tapdisk_message_name(message->type));
		}
	}

	if (message->u.params.flags & TAPDISK_MESSAGE_FLAG_OPEN_ENCRYPTED) {
		DPRINTF("Sending encryption key of %d bits\n", (int)key_size * 8);
		ret = write(sfd, &key_size, sizeof(key_size));
		if (ret != sizeof(key_size)) {
			EPRINTF("Failed to send encryption key size with '%s' message\n",
				tapdisk_message_name(message->type));
			return EIO;
		}
		ret = write(sfd, encryption_key, key_size);
		if (ret != key_size) {
			EPRINTF("Failed to send encryption key with '%s' message\n",
				tapdisk_message_name(message->type));
			return EIO;
		}
	}

	if (message->u.params.flags & TAPDISK_MESSAGE_FLAG_RATED) {
		DPRINTF("Sending socket for td-rated\n");
		char buf[TAPDISK_MESSAGE_MAX_PATH_LENGTH];
		snprintf(buf, TAPDISK_MESSAGE_MAX_PATH_LENGTH - 1, "%s", sockpath);

		ret = write(sfd,  &buf, sizeof(buf));

		if (ret == -1) {
			EPRINTF("Failed to send sockpath with '%s' message\n",
				tapdisk_message_name(message->type));
		}
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

	if (unlikely(strlen(name) >= sizeof(saddr.sun_path))) {
		EPRINTF("socket name too long: %s\n", name);
		close(fd);
		return -ENAMETOOLONG;
	}

	safe_strncpy(saddr.sun_path, name, sizeof(saddr.sun_path));

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

int
tap_ctl_connect_send_receive_ex(int id, tapdisk_message_t *message,
				const char *logpath,
				const char *sockpath,
				uint8_t key_size,
				const uint8_t *encryption_key,
				struct timeval *timeout)
{
	int err, sfd;

	err = tap_ctl_connect_id(id, &sfd);
	if (err)
		return err;

	err = tap_ctl_send_and_receive_ex(sfd, message, logpath, sockpath, key_size, encryption_key, timeout);

	close(sfd);
	return err;
}
