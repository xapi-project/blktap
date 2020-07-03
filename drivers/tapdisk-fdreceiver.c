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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/un.h>

#include "tapdisk.h"
#include "tapdisk-fdreceiver.h"
#include "tapdisk-server.h"
#include "timeout-math.h"
#include "scheduler.h"

#define UNIX_BUFFER_SIZE 16384

#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "nbd: " _f, ##_a)
#define ERROR(_f, _a...)           tlog_syslog(TLOG_WARN, "nbd: " _f, ##_a)

static void
td_fdreceiver_recv_fd(event_id_t id, char mode, void *data)
{
	struct td_fdreceiver *fdreceiver = data;
	int ret,  cv_flags = 0, *fdp, fd = -1;
	long numbytes;
	char *iobuf;
	char buf[CMSG_SPACE(sizeof(fd))];
	struct sockaddr_un unix_socket_name;

	struct msghdr msg;
	struct iovec vec;
	struct cmsghdr *cmsg;

	numbytes = UNIX_BUFFER_SIZE;
	iobuf = malloc(numbytes);
	if (!iobuf) {
		ERROR("Failed to allocate iobuf");
		return;
	}
	bzero(iobuf, numbytes);

	msg.msg_name = &unix_socket_name;
	msg.msg_namelen = sizeof(unix_socket_name);
	vec.iov_base = iobuf;
	vec.iov_len = numbytes;
	msg.msg_iov = &vec;

	msg.msg_iovlen = 1;

	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);

	ret = recvmsg(fdreceiver->client_fd, &msg, cv_flags);

	if (ret == -1) {
		ERROR("Failed to receive the message: %d", ret);
		goto out;
	}

	if (ret > 0 && msg.msg_controllen > 0) {
		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg->cmsg_level == SOL_SOCKET &&
				(cmsg->cmsg_type == SCM_RIGHTS)) {
			fdp = (int*)CMSG_DATA(cmsg);
			fd = *fdp;
		} else {
			ERROR("Failed to recieve a file descriptor");
		}
	} else {
		fd = -1;
	}

	if (ret < numbytes)
		numbytes = ret;

	INFO("Received fd %d with message: %s", fd, iobuf);

	/*
	 * We're done with this connection, it was only transiently used to
	 * connect the client
	 */
	close(fdreceiver->client_fd);
	fdreceiver->client_fd = -1;

	tapdisk_server_unregister_event(fdreceiver->client_event_id);
	fdreceiver->client_event_id = -1;

	/*
	 * It is the responsibility of this callback function to arrange that
	 * the fd is eventually closed
	 */
	fdreceiver->callback(fd, iobuf, fdreceiver->callback_data);
out:
	free(iobuf);
}

static void
td_fdreceiver_accept_fd(event_id_t id, char mode, void *data)
{
	struct sockaddr_storage their_addr;
	socklen_t sin_size = sizeof(their_addr);
	struct td_fdreceiver *fdreceiver = data;
	int new_fd;

	INFO("Unix domain socket is ready to accept");

	new_fd = accept(fdreceiver->fd,
			(struct sockaddr *)&their_addr, &sin_size);

	if (fdreceiver->client_fd != -1) {
		ERROR("td_fdreceiver_accept_fd: can only cope with one connec"
				"tion at once to the unix domain socket!");
		close(new_fd);
		return;
	}

	fdreceiver->client_fd = new_fd;

	fdreceiver->client_event_id =
		tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
				fdreceiver->client_fd, TV_ZERO,
				td_fdreceiver_recv_fd,
				fdreceiver);

	if (fdreceiver->client_event_id < 0) {
		ERROR("td_fdreceiver_accept_fd: failed to register event "
				"(errno=%d)", errno);
		close(new_fd);
		fdreceiver->client_fd = -1;
	}
}

void
td_fdreceiver_stop(struct td_fdreceiver *fdreceiver)
{
	if (fdreceiver->client_fd >= 0)
		close(fdreceiver->client_fd);

	if (fdreceiver->client_event_id >= 0)
		tapdisk_server_unregister_event(fdreceiver->client_event_id);

	if (fdreceiver->fd >= 0)
		close(fdreceiver->fd);

	if (fdreceiver->fd_event_id >= 0)
		tapdisk_server_unregister_event(fdreceiver->fd_event_id);

	if (fdreceiver->path != NULL) {
		unlink(fdreceiver->path);
		free(fdreceiver->path);
	}

	free(fdreceiver);
}

struct td_fdreceiver *
td_fdreceiver_start(char *path, fd_cb_t callback, void *data)
{
	int s = -1;
	struct sockaddr_un local;
	int len;
	int err;
	struct td_fdreceiver *fdreceiver;

	fdreceiver = malloc(sizeof(struct td_fdreceiver));
	if (!fdreceiver) {
		ERROR("td_fdreceiver_start: error allocating memory for "
				"fdreceiver (path=%s)", path);
		return NULL;
	}

	fdreceiver->path = strdup(path);
	if (unlikely(!fdreceiver->path)) {
		ERROR("td_fdreceiver_start: error allocating memory for "
				"fdreceiver->path (path=%s)", path);
		goto error;
	}

	fdreceiver->fd = -1;
	fdreceiver->fd_event_id = -1;
	fdreceiver->client_fd = -1;
	fdreceiver->client_event_id = -1;
	fdreceiver->callback = callback;
	fdreceiver->callback_data = data;

	err = snprintf(local.sun_path, sizeof(local.sun_path), "%s", path);
	if (unlikely(err >= sizeof(local.sun_path))) {
		ERROR("td_fdreceiver_start: socket name too long (path=%s)", path);
		goto error;
	} else if (unlikely(err < 0)) {
		ERROR("td_fdreceiver_start: snprintf() error (path=%s)", path);
		goto error;
	}
	local.sun_family = AF_UNIX;

	/*
	 * NB: here we unlink anything that was there before - be very careful
	 * with the paths you pass to this function!
	 */
	unlink(local.sun_path);
	len = strlen(local.sun_path) + sizeof(local.sun_family);

	s = socket(AF_UNIX, SOCK_STREAM, 0);

	if (s < 0) {
		ERROR("td_fdreceiver_start: error creating socket "
				"(path=%s)", path);
		goto error;
	}

	err = bind(s, (struct sockaddr *)&local, len);
	if (err < 0) {
		ERROR("td_fdreceiver_start: error binding (path=%s)", path);
		goto error;
	}

	err = listen(s, 5);
	if (err < 0) {
		ERROR("td_fdreceiver_start: error listening (path=%s)", path);
		goto error;
	}

	fdreceiver->fd = s;

	fdreceiver->fd_event_id =
		tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
				fdreceiver->fd, TV_ZERO,
				td_fdreceiver_accept_fd,
				fdreceiver);

	if (fdreceiver->fd_event_id < 0) {
		ERROR("td_fdreceiver_start: error registering event "
				"(path=%s)", path);
		goto error;
	}

	INFO("Set up local unix domain socket on path '%s'", path);

	return fdreceiver;

error:
	free(fdreceiver->path);
	free(fdreceiver);

	if (s >= 0)
		close(s);

	return NULL;
}
