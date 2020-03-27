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

#include <string.h>
#include <sys/types.h>

#include <wrappers.h>
#include "control-wrappers.h"
#include "util.h"
#include "test-suites.h"

#include "tap-ctl.h"
#include "blktap2.h"

void test_tap_ctl_list_success_no_results(void **state)
{
	int err;
	struct list_head list = LIST_HEAD_INIT(list);

	expect_string(__wrap_glob, pattern, "/sys/class/blktap2/blktap*");
	will_return(__wrap_glob, GLOB_NOMATCH);
	expect_string(__wrap_glob, pattern, "/var/run/blktap-control/ctl*");
	will_return(__wrap_glob, GLOB_NOMATCH);

	err = tap_ctl_list(&list);

	assert_int_equal(0, err);
	assert_true(list_empty(&list));
}


void test_tap_ctl_list_success_one_minor_no_td(void **state)
{
	int err;
	tap_list_t *entry;
	struct list_head list = LIST_HEAD_INIT(list);

	char *sys_glob_path = "/sys/class/blktap2/blktap!blktap0";
	char *sys_glob_data;
	char **sys_pathv = &sys_glob_data;

	sys_glob_data = test_malloc(strlen(sys_glob_path) + 2);
	memset(sys_glob_data, 0, strlen(sys_glob_path) + 2);
	strcpy(sys_glob_data, sys_glob_path);

	expect_string(__wrap_glob, pattern, "/sys/class/blktap2/blktap*");
	will_return(__wrap_glob, 0);
	will_return(__wrap_glob, 1);
	will_return(__wrap_glob, sys_pathv);
	expect_string(__wrap_glob, pattern, "/var/run/blktap-control/ctl*");
	will_return(__wrap_glob, GLOB_NOMATCH);

	/* Call API */
	err = tap_ctl_list(&list);

	assert_int_equal(0, err);
	assert_true(list_is_singular(&list));

	tap_list_for_each_entry(entry, &list) {
		assert_int_equal(0, entry->minor);
	}
	tap_ctl_list_free(&list);
}

void test_tap_ctl_list_success_one_td_no_minor_no_path(void **state)
{
	int err;
	tap_list_t *entry;
	struct list_head list = LIST_HEAD_INIT(list);

	pid_t test_pid = 1236;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1236";
	tapdisk_message_t write_message;
	tapdisk_message_t *read_message;
	struct mock_ipc_params *pid_ipc_params;
	struct mock_ipc_params *list_ipc_params;
	char *glob_path = "/var/run/blktap-control/ctl1236";
	char *glob_data;
	char **pathv = &glob_data;

	glob_data = test_malloc(strlen(glob_path) + 2);
	memset(glob_data, 0, strlen(glob_path) + 2);
	strcpy(glob_data, glob_path);

	expect_string(__wrap_glob, pattern, "/sys/class/blktap2/blktap*");
	will_return(__wrap_glob, GLOB_NOMATCH);
	expect_string(__wrap_glob, pattern, "/var/run/blktap-control/ctl*");
	will_return(__wrap_glob, 0);
	will_return(__wrap_glob, 1);
	will_return(__wrap_glob, pathv);

	/* IPC PID */
	memset(&write_message, 0, sizeof(write_message));
	write_message.type = TAPDISK_MESSAGE_PID;

	read_message = test_malloc(sizeof(*read_message));
	memset(read_message, 0, sizeof(*read_message));
	read_message->type = TAPDISK_MESSAGE_PID_RSP;
	read_message->u.tapdisk_pid = test_pid;

	pid_ipc_params = setup_ipc(
		expected_sock_name, ipc_socket,
		&write_message, read_message, 1);

	test_free(read_message);

	/* IPC List */
	memset(&write_message, 0, sizeof(write_message));
	write_message.type = TAPDISK_MESSAGE_LIST;
	write_message.cookie = -1;

	read_message = test_malloc(sizeof(*read_message) * 2);
	memset(read_message, 0, sizeof(*read_message) * 2);
	read_message[0].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[0].u.list.count = 1;
	read_message[0].u.list.minor = -1;
	read_message[0].u.list.state = -1;
	read_message[0].u.list.path[0] = 0;
	read_message[1].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[1].u.list.count = 0;
	read_message[1].u.list.minor = -1;
	read_message[1].u.list.state = -1;
	read_message[1].u.list.path[0] = 0;

	list_ipc_params = setup_ipc(
		expected_sock_name, ipc_socket,
		&write_message, read_message, 2);

	test_free(read_message);

	/* Call API */
	err = tap_ctl_list(&list);

	assert_int_equal(0, err);
	assert_true(list_is_singular(&list));

	tap_list_for_each_entry(entry, &list) {
		assert_int_equal(-1, entry->minor);
	}

	tap_ctl_list_free(&list);

	free_ipc_params(pid_ipc_params);
	free_ipc_params(list_ipc_params);
}

void test_tap_ctl_list_success_one_td_one_minor_no_path(void **state)
{
	int err;
	tap_list_t *entry;
	struct list_head list = LIST_HEAD_INIT(list);

	pid_t test_pid = 1236;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1236";
	tapdisk_message_t write_message;
	tapdisk_message_t *read_message;
	struct mock_ipc_params *pid_ipc_params;
	struct mock_ipc_params *list_ipc_params;
	char *sys_glob_path = "/sys/class/blktap2/blktap!blktap0";
	char *sys_glob_data;
	char **sys_pathv = &sys_glob_data;
	char *glob_path = "/var/run/blktap-control/ctl1236";
	char *glob_data;
	char **pathv = &glob_data;

	sys_glob_data = test_malloc(strlen(sys_glob_path) + 2);
	memset(sys_glob_data, 0, strlen(sys_glob_path) + 2);
	strcpy(sys_glob_data, sys_glob_path);

	glob_data = test_malloc(strlen(glob_path) + 2);
	memset(glob_data, 0, strlen(glob_path) + 2);
	strcpy(glob_data, glob_path);

	expect_string(__wrap_glob, pattern, "/sys/class/blktap2/blktap*");
	will_return(__wrap_glob, 0);
	will_return(__wrap_glob, 1);
	will_return(__wrap_glob, sys_pathv);
	expect_string(__wrap_glob, pattern, "/var/run/blktap-control/ctl*");
	will_return(__wrap_glob, 0);
	will_return(__wrap_glob, 1);
	will_return(__wrap_glob, pathv);

	/* IPC PID */
	memset(&write_message, 0, sizeof(write_message));
	write_message.type = TAPDISK_MESSAGE_PID;

	read_message = test_malloc(sizeof(*read_message));
	memset(read_message, 0, sizeof(*read_message));
	read_message->type = TAPDISK_MESSAGE_PID_RSP;
	read_message->u.tapdisk_pid = test_pid;

	pid_ipc_params = setup_ipc(
		expected_sock_name, ipc_socket,
		&write_message, read_message, 1);

	test_free(read_message);

	/* IPC List */
	memset(&write_message, 0, sizeof(write_message));
	write_message.type = TAPDISK_MESSAGE_LIST;
	write_message.cookie = -1;

	read_message = test_malloc(sizeof(*read_message) * 2);
	memset(read_message, 0, sizeof(*read_message) * 2);
	read_message[0].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[0].u.list.count = 1;
	read_message[0].u.list.minor = 0;
	read_message[0].u.list.state = 0;
	read_message[0].u.list.path[0] = 0;
	read_message[1].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[1].u.list.count = 0;
	read_message[1].u.list.minor = -1;
	read_message[1].u.list.state = -1;
	read_message[1].u.list.path[0] = 0;

	list_ipc_params = setup_ipc(
		expected_sock_name, ipc_socket,
		&write_message, read_message, 2);

	test_free(read_message);

	/* Call API */
	err = tap_ctl_list(&list);

	assert_int_equal(0, err);
	assert_true(list_is_singular(&list));

	tap_list_for_each_entry(entry, &list) {
		assert_int_equal(0, entry->minor);
	}

	tap_ctl_list_free(&list);

	free_ipc_params(pid_ipc_params);
	free_ipc_params(list_ipc_params);
}

void test_tap_ctl_list_success(void **state)
{
	int err;
	tap_list_t *entry;
	struct list_head list = LIST_HEAD_INIT(list);

	pid_t test_pid = 1236;
	int ipc_socket = 7;
	char *expected_sock_name = "/var/run/blktap-control/ctl1236";
	tapdisk_message_t write_message;
	tapdisk_message_t *read_message;
	struct mock_ipc_params *pid_ipc_params;
	struct mock_ipc_params *list_ipc_params;
	char *sys_glob_path = "/sys/class/blktap2/blktap!blktap0";
	char *sys_glob_data;
	char **sys_pathv = &sys_glob_data;
	char *glob_path = "/var/run/blktap-control/ctl1236";
	char *glob_data;
	char **pathv = &glob_data;
	char *vdi_path =
		"vhd:/dev/VG_XenStorage-a201d430-bc48-fd74-1aeb-292514d9afd6/"
		"VHD-b1c552a0-e0e6-4156-b6fe-18d7d114c6be";

	sys_glob_data = test_malloc(strlen(sys_glob_path) + 2);
	memset(sys_glob_data, 0, strlen(sys_glob_path) + 2);
	strcpy(sys_glob_data, sys_glob_path);

	glob_data = test_malloc(strlen(glob_path) + 2);
	memset(glob_data, 0, strlen(glob_path) + 2);
	strcpy(glob_data, glob_path);

	expect_string(__wrap_glob, pattern, "/sys/class/blktap2/blktap*");
	will_return(__wrap_glob, 0);
	will_return(__wrap_glob, 1);
	will_return(__wrap_glob, sys_pathv);
	expect_string(__wrap_glob, pattern, "/var/run/blktap-control/ctl*");
	will_return(__wrap_glob, 0);
	will_return(__wrap_glob, 1);
	will_return(__wrap_glob, pathv);

	/* IPC PID */
	memset(&write_message, 0, sizeof(write_message));
	write_message.type = TAPDISK_MESSAGE_PID;

	read_message = test_malloc(sizeof(*read_message));
	memset(read_message, 0, sizeof(*read_message));
	read_message->type = TAPDISK_MESSAGE_PID_RSP;
	read_message->u.tapdisk_pid = test_pid;

	pid_ipc_params = setup_ipc(
		expected_sock_name, ipc_socket,
		&write_message, read_message, 1);

	test_free(read_message);

	/* IPC List */
	memset(&write_message, 0, sizeof(write_message));
	write_message.type = TAPDISK_MESSAGE_LIST;
	write_message.cookie = -1;

	read_message = test_malloc(sizeof(*read_message) * 2);
	memset(read_message, 0, sizeof(*read_message) * 2);
	read_message[0].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[0].u.list.count = 1;
	read_message[0].u.list.minor = 0;
	read_message[0].u.list.state = 0;
	strcpy(read_message[0].u.list.path, vdi_path);
	read_message[1].type = TAPDISK_MESSAGE_LIST_RSP;
	read_message[1].u.list.count = 0;
	read_message[1].u.list.minor = -1;
	read_message[1].u.list.state = -1;
	read_message[1].u.list.path[0] = 0;

	list_ipc_params = setup_ipc(
		expected_sock_name, ipc_socket,
		&write_message, read_message, 2);

	test_free(read_message);

	/* Call API */
	err = tap_ctl_list(&list);

	assert_int_equal(0, err);
	assert_true(list_is_singular(&list));

	tap_list_for_each_entry(entry, &list) {
		assert_int_equal(0, entry->minor);
		assert_string_equal("vhd", entry->type);
		assert_string_equal(
			"/dev/VG_XenStorage-a201d430-bc48-fd74-1aeb-292514d9afd6/"
			"VHD-b1c552a0-e0e6-4156-b6fe-18d7d114c6be", entry->path);
	}

	tap_ctl_list_free(&list);

	free_ipc_params(pid_ipc_params);
	free_ipc_params(list_ipc_params);
}
