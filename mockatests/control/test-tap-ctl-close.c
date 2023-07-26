/*
 * Copyright (c) 2017, Citrix Systems, Inc.
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

#include <string.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>

#include <wrappers.h>
#include "control-wrappers.h"
#include "util.h"
#include "test-suites.h"

#include "tap-ctl.h"
#include "blktap2.h"

void test_tap_ctl_close_success(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct mock_select_params write_select_params;
	struct mock_select_params read_select_params;
	struct mock_read_params read_params;
	tapdisk_message_t write_message;
	tapdisk_message_t read_message;
	struct sockaddr_un saddr;

	initialise_select_params(&write_select_params);
	initialise_select_params(&read_select_params);
	memset(&read_message, 0, sizeof(read_message));
	memset(&write_message, 0, sizeof(write_message));

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, 0);

	write_select_params.result = 1;
	FD_SET(ipc_socket, &write_select_params.writefds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &write_select_params);

	write_message.type = TAPDISK_MESSAGE_CLOSE;
	write_message.cookie = test_minor;
	expect_value(__wrap_write, fd, ipc_socket);
	expect_memory(__wrap_write, buf, &write_message, sizeof(write_message));
	will_return(__wrap_write, 1024);

	read_select_params.result = 1;
	FD_SET(ipc_socket, &read_select_params.readfds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &read_select_params);

	read_message.type = TAPDISK_MESSAGE_CLOSE_RSP;
	read_message.cookie = test_minor;
	read_message.u.response.error = 0;
	read_params.result = sizeof(read_message);
	read_params.data = &read_message;
	expect_value(__wrap_read, fd, ipc_socket);
	will_return(__wrap_read, &read_params);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, 0, NULL);

	assert_int_equal(0, result);
}

void test_tap_ctl_force_close_success(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct mock_select_params write_select_params;
	struct mock_select_params read_select_params;
	struct mock_read_params read_params;
	tapdisk_message_t write_message;
	tapdisk_message_t read_message;
	struct sockaddr_un saddr;

	initialise_select_params(&write_select_params);
	initialise_select_params(&read_select_params);
	memset(&read_message, 0, sizeof(read_message));
	memset(&write_message, 0, sizeof(write_message));

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, 0);

	write_select_params.result = 1;
	FD_SET(ipc_socket, &write_select_params.writefds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &write_select_params);

	write_message.type = TAPDISK_MESSAGE_FORCE_SHUTDOWN;
	write_message.cookie = test_minor;
	expect_value(__wrap_write, fd, ipc_socket);
	expect_memory(__wrap_write, buf, &write_message, sizeof(write_message));
	will_return(__wrap_write, 1024);

	read_select_params.result = 1;
	FD_SET(ipc_socket, &read_select_params.readfds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &read_select_params);

	read_message.type = TAPDISK_MESSAGE_CLOSE_RSP;
	read_message.cookie = test_minor;
	read_message.u.response.error = 0;
	read_params.result = sizeof(read_message);
	read_params.data = &read_message;
	expect_value(__wrap_read, fd, ipc_socket);
	will_return(__wrap_read, &read_params);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, -1, NULL);

	assert_int_equal(0, result);
}

void test_tap_ctl_close_connect_fail(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct sockaddr_un saddr;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, ENOENT);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, 0, NULL);

	assert_int_equal(-ENOENT, result);
}

void test_tap_ctl_close_write_error(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct mock_select_params write_select_params;
	tapdisk_message_t message;
	struct sockaddr_un saddr;

	initialise_select_params(&write_select_params);
	memset(&message, 0, sizeof(message));

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, 0);

	write_select_params.result = 1;
	FD_SET(ipc_socket, &write_select_params.writefds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &write_select_params);

	expect_value(__wrap_write, fd, ipc_socket);
	expect_any(__wrap_write, buf);
	will_return(__wrap_write, -EIO);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, 0, NULL);

	assert_int_equal(-EIO, result);
}

void test_tap_ctl_close_read_error(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct mock_select_params write_select_params;
	struct mock_select_params read_select_params;
	struct mock_read_params read_params;
	tapdisk_message_t write_message;
	struct sockaddr_un saddr;

	initialise_select_params(&write_select_params);
	initialise_select_params(&read_select_params);
	memset(&write_message, 0, sizeof(write_message));

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, 0);

	write_select_params.result = 1;
	FD_SET(ipc_socket, &write_select_params.writefds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &write_select_params);

	write_message.type = TAPDISK_MESSAGE_CLOSE;
	write_message.cookie = test_minor;
	expect_value(__wrap_write, fd, ipc_socket);
	expect_memory(__wrap_write, buf, &write_message, sizeof(write_message));
	will_return(__wrap_write, 1024);

	read_select_params.result = 1;
	FD_SET(ipc_socket, &read_select_params.readfds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &read_select_params);

	read_params.result = -EIO;
	read_params.data = NULL;
	expect_value(__wrap_read, fd, ipc_socket);
	will_return(__wrap_read, &read_params);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, 0, NULL);

	assert_int_equal(-EIO, result);
}

void test_tap_ctl_close_write_select_timeout(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct mock_select_params write_select_params;
	struct timeval write_timeout;
	tapdisk_message_t message;
	struct sockaddr_un saddr;

	initialise_select_params(&write_select_params);
	memset(&message, 0, sizeof(message));

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, 0);

	write_timeout.tv_sec = 30;
	write_timeout.tv_usec = 0;
	write_select_params.result = 0;
	expect_memory(__wrap_select, timeout, &write_timeout, sizeof(write_timeout));
	will_return(__wrap_select, &write_select_params);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, 0, &write_timeout);

	assert_int_equal(-EIO, result);
}

void test_tap_ctl_close_read_select_timeout(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct mock_select_params write_select_params;
	struct mock_select_params read_select_params;
	struct timeval write_timeout;
	struct timeval read_timeout;
	tapdisk_message_t write_message;
	struct sockaddr_un saddr;

	initialise_select_params(&write_select_params);
	initialise_select_params(&read_select_params);
	memset(&write_message, 0, sizeof(write_message));

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, 0);

	write_timeout.tv_sec = 30;
	write_timeout.tv_usec = 0;
	write_select_params.result = 1;
	FD_SET(ipc_socket, &write_select_params.writefds);
	expect_memory(__wrap_select, timeout, &write_timeout, sizeof(write_timeout));
	will_return(__wrap_select, &write_select_params);

	write_message.type = TAPDISK_MESSAGE_CLOSE;
	write_message.cookie = test_minor;
	expect_value(__wrap_write, fd, ipc_socket);
	expect_memory(__wrap_write, buf, &write_message, sizeof(write_message));
	will_return(__wrap_write, 1024);

	read_timeout.tv_sec = 30;
	read_timeout.tv_usec = 0;
	read_select_params.result = 0;
	expect_memory(__wrap_select, timeout, &read_timeout, sizeof(read_timeout));
	will_return(__wrap_select, &read_select_params);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, 0, &write_timeout);

	assert_int_equal(-ETIMEDOUT, result);
}

void test_tap_ctl_close_error_response(void **state)
{
	int result;
	int test_pid = 1345;
	int test_minor = 17;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1345";
	struct mock_select_params write_select_params;
	struct mock_select_params read_select_params;
	struct mock_read_params read_params;
	tapdisk_message_t write_message;
	tapdisk_message_t read_message;
	struct sockaddr_un saddr;

	initialise_select_params(&write_select_params);
	initialise_select_params(&read_select_params);
	memset(&read_message, 0, sizeof(read_message));
	memset(&write_message, 0, sizeof(write_message));

	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strcpy(saddr.sun_path, expected_sock_name);

	expect_value(__wrap_socket, domain, AF_UNIX);
	expect_value(__wrap_socket, type, SOCK_STREAM);
	expect_any(__wrap_socket, protocol);
	will_return(__wrap_socket, ipc_socket);

	expect_value(__wrap_connect, sockfd, ipc_socket);
	expect_memory(__wrap_connect, addr, &saddr, sizeof(saddr));
	will_return(__wrap_connect, 0);

	write_select_params.result = 1;
	FD_SET(ipc_socket, &write_select_params.writefds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &write_select_params);

	write_message.type = TAPDISK_MESSAGE_CLOSE;
	write_message.cookie = test_minor;
	expect_value(__wrap_write, fd, ipc_socket);
	expect_memory(__wrap_write, buf, &write_message, sizeof(write_message));
	will_return(__wrap_write, 1024);

	read_select_params.result = 1;
	FD_SET(ipc_socket, &read_select_params.readfds);
	expect_value(__wrap_select, timeout, NULL);
	will_return(__wrap_select, &read_select_params);

	read_message.type = TAPDISK_MESSAGE_ERROR;
	read_message.cookie = test_minor;
	read_message.u.response.error = ENOENT;
	read_params.result = sizeof(read_message);
	read_params.data = &read_message;
	expect_value(__wrap_read, fd, ipc_socket);
	will_return(__wrap_read, &read_params);

	expect_value(__wrap_close, fd, ipc_socket);
	will_return(__wrap_close, 0);

	/* Call test API */
	result = tap_ctl_close(test_pid, test_minor, 0, NULL);

	assert_int_equal(-ENOENT, result);
}
