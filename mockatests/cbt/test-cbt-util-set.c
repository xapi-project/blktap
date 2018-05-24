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
#define __STDC_FORMAT_MACROS

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <inttypes.h>

#include <cbt-util-priv.h>
#include <wrappers.h>
#include "test-suites.h"

int __real_printf(const char *format, ...);

void
test_cbt_util_set_parent(void **state)
{
	int result;
	void *log_meta;
	uuid_t parent;
	char uuid_str[36];
	char uuid_str_after[36];

	uuid_generate_random(parent);
	uuid_unparse(parent, uuid_str);
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-p", uuid_str};

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, 0);

	uuid_unparse(((struct cbt_log_metadata*)log_meta)->parent, uuid_str_after);
	assert_string_equal(uuid_str, uuid_str_after);

	free(log_meta);
}

void
test_cbt_util_set_child(void **state)
{
	int result;
	void *log_meta;
	uuid_t child;
	char uuid_str[36];
	char uuid_str_after[36];

	uuid_generate_random(child);
	uuid_unparse(child, uuid_str);
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-c", uuid_str};

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, 0);

	uuid_unparse(((struct cbt_log_metadata*)log_meta)->child, uuid_str_after);
	assert_string_equal(uuid_str, uuid_str_after);

	free(log_meta);
}

void
test_cbt_util_set_flag(void **state)
{
	int result;
	void *log_meta;
	char flag_string[1] = "2";
	int flag_int = 2;
	int log_flag;
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-f", flag_string};

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, 0);

	log_flag = ((struct cbt_log_metadata*)log_meta)->consistent;
	assert_int_equal(flag_int, log_flag);

	free(log_meta);
}

void
test_cbt_util_set_size(void **state)
{
	int result;
	void *log_meta;
	uint64_t size_int64 = 4194303;
	char size_string[8];
	snprintf(size_string, sizeof(size_string), "%" PRIu64, size_int64);
	uint64_t log_size_int64;
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-s", size_string};

	uint64_t btmsize = bitmap_size(size_int64);

	int file_size = sizeof(struct cbt_log_metadata) + btmsize;
	log_meta = malloc(file_size);

	((struct cbt_log_metadata*)log_meta)->size = 2048;

	memcpy(log_meta + sizeof(struct cbt_log_metadata), (void*)memcpy, btmsize);
	FILE *test_log = fmemopen((void*)log_meta, file_size, "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, 0);

	log_size_int64 = ((struct cbt_log_metadata*)log_meta)->size;
	
	assert_true(log_size_int64 == size_int64);

	free(log_meta);
}

void
test_cbt_util_set_size_smaller_file_failure(void **state)
{
	int result;
	void *log_meta;
	uint64_t size_int64 = 2048;
	char size_string[8];
	snprintf(size_string, sizeof(size_string), "%" PRIu64, size_int64);
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-s", size_string};


	log_meta = malloc(sizeof(struct cbt_log_metadata));

	((struct cbt_log_metadata*)log_meta)->size = 4096;

	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, -EINVAL);

	free(log_meta);
}

void
test_cbt_util_set_size_malloc_failure(void **state)
{
	int result;
	uint64_t size_int64 = 4096;
	int file_size;
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-s", "4194303"};
	void *log_meta;

	uint64_t btmsize = bitmap_size(size_int64);

	file_size = sizeof(struct cbt_log_metadata) + 4096;
	log_meta = malloc(file_size);

	((struct cbt_log_metadata*)log_meta)->size = 4096;
	
	memcpy(log_meta + sizeof(struct cbt_log_metadata), (void*)memcpy, btmsize);
	FILE *test_log = fmemopen((void*)log_meta, file_size, "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);	
		
	malloc_succeeds(true);
	malloc_succeeds(false);

	result = cbt_util_set(5, args);
	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
	free(log_meta);
}

void
test_cbt_util_set_size_no_bitmap_failure(void **state)
{
	int result;
	void *log_meta;
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-s", "4194304"};

	log_meta = malloc(sizeof(struct cbt_log_metadata));
	((struct cbt_log_metadata*)log_meta)->size = 4096;

	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, -EIO);

	free(log_meta);
}

void
test_cbt_util_set_size_write_failure(void **state)
{
	int result;
	void *log_meta;
	uint64_t size_int64 = 4194303;
	char size_string[8];
	snprintf(size_string, sizeof(size_string), "%" PRIu64, size_int64);
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-s", size_string};

	uint64_t btmsize = bitmap_size(size_int64);

	int file_size = sizeof(struct cbt_log_metadata) + btmsize;
	log_meta = malloc(file_size);

	((struct cbt_log_metadata*)log_meta)->size = 4096;

	memcpy(log_meta + sizeof(struct cbt_log_metadata), (void*)memcpy, btmsize);
	FILE *test_log = fmemopen((void*)log_meta, file_size, "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, -EIO);

	free(log_meta);
}

void
test_cbt_util_set_size_reset_file_pointer_failure(void **state)
{
	int result;
	void *log_meta;
	uint64_t size_int64 = 4194303;
	char size_string[8];
	snprintf(size_string, sizeof(size_string), "%" PRIu64, size_int64);
	char* args[] = {"cbt-util", "-n", "test_disk.log", "-s", size_string};

	uint64_t btmsize = bitmap_size(size_int64);

	int file_size = sizeof(struct cbt_log_metadata) + btmsize;
	log_meta = malloc(file_size);

	((struct cbt_log_metadata*)log_meta)->size = 2048;

	memcpy(log_meta + sizeof(struct cbt_log_metadata), (void*)memcpy, btmsize);
	FILE *test_log = fmemopen((void*)log_meta, file_size, "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	fail_fseek(EIO);

	result = cbt_util_set(5, args);
	assert_int_equal(result, -EIO);

	free(log_meta);
}

void
test_cbt_util_set_no_name_failure(void **state)
{
	int result;
	char* args[] = {"cbt-util", "-p"};

	struct printf_data *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_set(2, args);
	assert_int_equal(result, -EINVAL);
	free_printf_data(output);
}

void
test_cbt_util_set_no_command_failure(void **state)
{
	int result;
	char* args[] = {"cbt-util", "-n", "test_disk.log"};
	struct printf_data *output;

	disable_malloc_mock();
	output = setup_vprintf_mock(1024);

	result = cbt_util_set(3, args);
	assert_int_equal(result, -EINVAL);
	free_printf_data(output);
}

void
test_cbt_util_set_malloc_failure(void **state)
{
	int result;
	uuid_t parent;
	char uuid_str[36];
	uuid_generate_random(parent);
	uuid_unparse(parent, uuid_str);

	char* args[] = {"cbt-util", "-n", "test_disk.log", "-p", uuid_str};

	malloc_succeeds(false);

	result = cbt_util_set(5, args);
	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
}

void
test_cbt_util_set_no_data_failure(void **state)
{
	int result;
	uuid_t parent;
	char uuid_str[36];
	uuid_generate_random(parent);
	uuid_unparse(parent, uuid_str);

	char* args[] = {"cbt-util", "-n", "test_disk.log", "-p", uuid_str};
	void *log_meta[1];

	FILE *test_log = fmemopen((void*)log_meta, 1, "r+");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_set(5, args);
	assert_int_equal(result, -EIO);
}

void
test_cbt_util_set_no_file_failure(void **state)
{
	int result;
	uuid_t parent;
	char uuid_str[36];
	uuid_generate_random(parent);
	uuid_unparse(parent, uuid_str);


	char* args[] = {"cbt-util", "-n", "test_disk.log", "-p", uuid_str};

	will_return(__wrap_fopen, NULL);

	result = cbt_util_set(5, args);

	assert_int_equal(result, -ENOENT);
}
