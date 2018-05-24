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
#include <errno.h>

#include <cbt-util-priv.h>
#include "wrappers.h"
#include "test-suites.h"

void test_cbt_util_create_success(void **state)
{
	int result;
	int file_size;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-s", "4194304" };
	void *log_meta;

	file_size = 4194304 + sizeof(struct cbt_log_metadata);

	log_meta = test_malloc(file_size);
	FILE *test_log = fmemopen(log_meta, file_size, "w+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_create(5, args);

	assert_int_equal(result, 0);
	assert_int_equal(((struct cbt_log_metadata*)log_meta)->consistent, 0);

	test_free(log_meta);
}

void test_cbt_util_create_file_open_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-s", "4194304" };

	will_return(__wrap_fopen, NULL);
	/* TODO: Could do with this then making another call to mock to get the error code */

	result = cbt_util_create(5, args);

	assert_int_equal(result, -ENOENT);
}

void test_cbt_util_create_metadata_write_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-s", "4194304" };
	void *log_meta[1];

	FILE *test_log = fmemopen(log_meta, 1, "w+");

	/* Make IO errors happen immediately, not on flush */
	setbuf(test_log, NULL);

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_create(5, args);

	assert_int_equal(result, -EIO);
}

void test_cbt_util_create_bitmap_write_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-s", "4194304" };
	void *log_meta;

	log_meta = test_malloc(sizeof(struct cbt_log_metadata));

	FILE *test_log = fmemopen(log_meta, sizeof(struct cbt_log_metadata), "w+");

	/* Make IO errors happen immediately, not on flush */
	setbuf(test_log, NULL);

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_create(5, args);

	assert_int_equal(result, -EIO);

	test_free(log_meta);
}

void test_cbt_util_create_log_data_allocation_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-s", "4194304" };

	malloc_succeeds(false);

	result = cbt_util_create(5, args);

	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
}

void test_cbt_util_create_bitmap_allocation_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-s", "4194304" };

	malloc_succeeds(true);
	malloc_succeeds(false);

	result = cbt_util_create(5, args);

	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
}

void test_cbt_util_create_no_name_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-s", "4194304" };
	struct printf_data *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_create(3, args);

	assert_int_equal(result, -EINVAL);

	free_printf_data(output);
}

void test_cbt_util_create_no_size_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log" };
	struct printf_data *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_create(3, args);

	assert_int_equal(result, -EINVAL);

	free_printf_data(output);
}

