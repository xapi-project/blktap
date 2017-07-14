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
#include <setjmp.h>
#include <cmocka.h>

#include <cbt-util-priv.h>
#include <wrappers.h>
#include "test-suites.h"

void
test_get_command_create(void **state)
{
	struct command *cmd;

	char* requested_command = { "create" };

	cmd = get_command(requested_command);

	assert_string_equal(cmd->name, "create");
	assert_ptr_equal(cmd->func, cbt_util_create);
}

void
test_get_command_set(void **state)
{
	struct command *cmd;

	char* requested_command = { "set" };

	cmd = get_command(requested_command);

	assert_string_equal(cmd->name, "set");
	assert_ptr_equal(cmd->func, cbt_util_set);
}

void
test_get_command_get(void **state)
{
	struct command *cmd;

	char* requested_command = { "get" };

	cmd = get_command(requested_command);

	assert_string_equal(cmd->name, "get");
	assert_ptr_equal(cmd->func, cbt_util_get);
}

void
test_get_command_bad_command(void **state)
{
	struct command *cmd;

	char* requested_command = { "breakme" };

	cmd = get_command(requested_command);

	assert_null(cmd);
}

void
test_get_command_over_long_command(void **state)
{
	struct command *cmd;

	char* requested_command = { "im_a_really_really_long_command" };

	cmd = get_command(requested_command);

	assert_null(cmd);
}

void
test_help_success(void ** state)
{
	struct printf_data *output;

	output = setup_vprintf_mock(1024);

	help();

	assert_in_range(output->offset, 10, 1024);

	free_printf_data(output);
}
