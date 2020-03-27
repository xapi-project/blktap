/*
 * Copyright (c) 2020, Citrix Systems, Inc.
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

#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>

#include <sys/un.h>
#include <sys/socket.h>

#include "util.h"

struct mock_ipc_params
{
	struct mock_select_params write_select_params;
	tapdisk_message_t write_message;
	int read_message_count;
	struct mock_select_params *read_select_params;
	tapdisk_message_t *read_message;
	struct mock_read_params *read_params;
	struct sockaddr_un saddr;
};

void initialise_select_params(struct mock_select_params *params)
{
	FD_ZERO(&params->readfds);
	FD_ZERO(&params->writefds);
	FD_ZERO(&params->exceptfds);
}

struct mock_ipc_params *setup_ipc(char *ipc_socket_name, int ipc_socket_fd,
				  tapdisk_message_t *write_message,
				  tapdisk_message_t *read_message,
				  int read_message_count)
{
	struct mock_ipc_params *ipc_params;
	int id;

	ipc_params = test_malloc(sizeof(struct mock_ipc_params));
	ipc_params->read_message_count = read_message_count;
	ipc_params->read_select_params = test_malloc(read_message_count * sizeof(struct mock_select_params));
	ipc_params->read_message = test_malloc(read_message_count * sizeof(tapdisk_message_t));
	ipc_params->read_params = test_malloc(read_message_count * sizeof(struct mock_read_params));

	initialise_select_params(&(ipc_params->write_select_params));

	memcpy(&(ipc_params->write_message), write_message, sizeof(tapdisk_message_t));

	memset(&(ipc_params->saddr), 0, sizeof(ipc_params->saddr));
	ipc_params->saddr.sun_family = AF_UNIX;
	strcpy(ipc_params->saddr.sun_path, ipc_socket_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket_fd);

	expect_value(__wrap_connect, sockfd, ipc_socket_fd);
	expect_memory(__wrap_connect, addr,
		      &(ipc_params->saddr), sizeof(ipc_params->saddr));
	will_return(__wrap_connect, 0);

	ipc_params->write_select_params.result = 1;
	FD_SET(ipc_socket_fd, &(ipc_params->write_select_params.writefds));
	expect_any(__wrap_select, timeout);
	will_return(__wrap_select, &(ipc_params->write_select_params));

	expect_value(__wrap_write, fd, ipc_socket_fd);
	expect_memory(__wrap_write, buf,
		      &(ipc_params->write_message), sizeof(tapdisk_message_t));
	will_return(__wrap_write, 1024);

	/* Add Read responses */
	for (id = 0; id < read_message_count; id++) {
		initialise_select_params(&(ipc_params->read_select_params[id]));
		ipc_params->read_select_params[id].result = 1;
		FD_SET(ipc_socket_fd, &(ipc_params->read_select_params[id].readfds));
		expect_any(__wrap_select, timeout);
		will_return(__wrap_select, &(ipc_params->read_select_params[id]));

		memcpy(&(ipc_params->read_message[id]),
		       &(read_message[id]),
		       sizeof(tapdisk_message_t));

		ipc_params->read_params[id].result = sizeof(*read_message);
		ipc_params->read_params[id].data = &(ipc_params->read_message[id]);
		expect_value(__wrap_read, fd, ipc_socket_fd);
		will_return(__wrap_read, &(ipc_params->read_params[id]));
	}

	expect_value(__wrap_close, fd, ipc_socket_fd);
	will_return(__wrap_close, 0);

	return ipc_params;
}

void free_ipc_params(struct mock_ipc_params *params)
{
	test_free(params->read_message);
	test_free(params->read_select_params);
	test_free(params->read_params);
	test_free(params);
}

