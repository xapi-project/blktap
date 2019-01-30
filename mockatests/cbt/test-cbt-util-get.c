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

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <uuid/uuid.h>

#include <cbt-util-priv.h>
#include <wrappers.h>
#include "test-suites.h"

void test_cbt_util_get_flag(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-f" };
	void *log_meta;
	struct printf_data *output;

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	((struct cbt_log_metadata*)log_meta)->consistent = 1;
	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	output = setup_vprintf_mock(1024);

	result = cbt_util_get(4, args);

	assert_int_equal(result, 0);
	assert_string_equal(output->buf, "1\n");
	free_printf_data(output);
	free(log_meta);
}

void test_cbt_util_get_parent(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-p" };
	void *log_meta;
	struct printf_data *output;
	uuid_t parent;
	char uuid_str[38];

	uuid_generate_random(parent);

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	uuid_copy(((struct cbt_log_metadata*)log_meta)->parent, parent);
	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	output = setup_vprintf_mock(1024);

	result = cbt_util_get(4, args);

	assert_int_equal(result, 0);
	uuid_unparse(parent, uuid_str);
	strncat(uuid_str, "\n", 37);

	assert_string_equal(output->buf, uuid_str);
	free_printf_data(output);
	free(log_meta);
}

void test_cbt_util_get_child(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-c" };
	void *log_meta;
	struct printf_data *output;
	uuid_t child;
	char uuid_str[38];

	uuid_generate_random(child);

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	uuid_copy(((struct cbt_log_metadata*)log_meta)->child, child);
	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	output = setup_vprintf_mock(1024);

	result = cbt_util_get(4, args);

	assert_int_equal(result, 0);
	uuid_unparse(child, uuid_str);
	strncat(uuid_str, "\n", 37);

	assert_string_equal(output->buf, uuid_str);
	free_printf_data(output);
	free(log_meta);
}

void test_cbt_util_get_size(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-s" };
	void *log_meta;
	struct printf_data *output;

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	((struct cbt_log_metadata*)log_meta)->size = 4194304;
	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	output = setup_vprintf_mock(1024);

	result = cbt_util_get(4, args);

	assert_int_equal(result, 0);
	assert_string_equal(output->buf, "4194304\n");
	free_printf_data(output);
	free(log_meta);
}

void test_cbt_util_get_bitmap(void **state)
{
	int result;
	int file_size;
	char* args[] = { "cbt-util", "get", "-b", "-n", "test_disk.log" };
	void *log_meta;
	struct fwrite_data *output;
	uint64_t size = 4194304;

	uint64_t bmsize = bitmap_size(size);
	file_size = sizeof(struct cbt_log_metadata) + bmsize;
	log_meta = malloc(file_size);

	//Intialise size in metadata file
	((struct cbt_log_metadata*)log_meta)->size = size;
	//Fill bitmap with random bytes
	memcpy( log_meta + sizeof(struct cbt_log_metadata), (void*)memcpy, bmsize );
	FILE *test_log = fmemopen((void*)log_meta, file_size, "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);
	enable_mock_fwrite();
	output = setup_fwrite_mock(bmsize);

	result = cbt_util_get(5, args);
	assert_int_equal(result, 0);
	assert_memory_equal(output->buf, log_meta + sizeof(struct cbt_log_metadata), bmsize);

	free_fwrite_data(output);
	free(log_meta);
}

void test_cbt_util_get_bitmap_nodata_failure(void **state)
{

	int result;
	char* args[] = { "cbt-util", "get", "-b", "-n", "test_disk.log" };
	void *log_meta;
	uint64_t size = 4194304;

	log_meta = malloc(sizeof(struct cbt_log_metadata));
	//Intialise size in metadata file
	((struct cbt_log_metadata*)log_meta)->size = size;
	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_get(5, args);
	assert_int_equal(result, -EIO);

	free(log_meta);

}

void test_cbt_util_get_bitmap_malloc_failure(void **state)
{
	int result;
	int file_size;
	char* args[] = { "cbt-util", "get", "-b", "-n", "test_disk.log" };
	void *log_meta;

	file_size = 4194304 + sizeof(struct cbt_log_metadata);
	log_meta = malloc(file_size);
	FILE *test_log = fmemopen((void*)log_meta, file_size, "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	malloc_succeeds(true);
	malloc_succeeds(false);

	result = cbt_util_get(5, args);
	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
	free(log_meta);
}

void test_cbt_util_get_no_bitmap_flag_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "get", "-n", "test_disk.log" };
	struct printf_data *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_get(4, args);
	assert_int_equal(result, -EINVAL);
	free_printf_data(output);
}

void test_cbt_util_get_nofile_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-c" };

	will_return(__wrap_fopen, NULL);

	result = cbt_util_get(4, args);

	assert_int_equal(result, -ENOENT);
}

void test_cbt_util_get_nodata_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-c" };
	void *log_meta[1];

	FILE *test_log = fmemopen((void*)log_meta, 1, "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_get(4, args);

	assert_int_equal(result, -EIO);
}

void test_cbt_util_get_malloc_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-c" };

	malloc_succeeds(false);

	result = cbt_util_get(4, args);
	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
}

void test_cbt_util_get_no_name_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-c" };
	struct printf_data *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_get(2, args);
	assert_int_equal(result, -EINVAL);
	free_printf_data(output);
}

void test_cbt_util_get_no_command_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log" };
	struct printf_data  *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_get(3, args);
	assert_int_equal(result, -EINVAL);
	free_printf_data(output);
}
