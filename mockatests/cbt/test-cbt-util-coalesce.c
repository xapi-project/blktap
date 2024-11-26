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

#include <cbt-util-priv.h>
#include <wrappers.h>
#include "test-suites.h"

/*
 * Test failure when no parent parameter provided
 */
void test_cbt_util_coalesce_no_parent_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-c", "test_child.log" };
	struct printf_data *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_coalesce(4, args);
	assert_int_equal(result, -EINVAL);
	free_printf_data(output);
}

/*
 * Test failure when no child parameter provided
 */
void test_cbt_util_coalesce_no_child_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log" };
	struct printf_data  *output;

	output = setup_vprintf_mock(1024);

	result = cbt_util_coalesce(4, args);
	assert_int_equal(result, -EINVAL);
	free_printf_data(output);
}

/*
 * Test failure when parent log file doesn't exist
 */
void test_cbt_util_coalesce_no_parent_file_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};

	will_return(__wrap_fopen, NULL);

	result = cbt_util_coalesce(6, args);

	assert_int_equal(result, -ENOENT);
}

/*
 * Test failure when child log file doesn't exist
 */
void test_cbt_util_coalesce_no_child_file_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};

	void *log_meta;

	log_meta = malloc(sizeof(struct cbt_log_metadata));
	FILE *parent_log = fmemopen((void*)log_meta,
								sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);

	will_return(__wrap_fopen, NULL);

	result = cbt_util_coalesce(6, args);

	assert_int_equal(result, -ENOENT);
	free(log_meta);
}

/*
 * Test failure to malloc cbt_log_metadata structure for parent
 */
void test_cbt_util_coalesce_parent_log_malloc_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *log_meta[1];
	FILE *test_log = fmemopen((void*)log_meta, 1, "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	malloc_succeeds(false);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
}

/*
 * Test failure to malloc cbt_log_metadata structure for child
 */
void test_cbt_util_coalesce_child_log_malloc_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	
	void *log_meta;

	log_meta = malloc(sizeof(struct cbt_log_metadata));
	FILE *parent_log = fmemopen((void*)log_meta,
								sizeof(struct cbt_log_metadata), "r");
	FILE *child_log = fmemopen((void*)log_meta,
								sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);

	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	malloc_succeeds(true);
	malloc_succeeds(false);

	result = cbt_util_coalesce(6, args);

	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
	free(log_meta);
}

/*
 * Test failure to read log metadata for parent
 */
void test_cbt_util_coalesce_no_parent_meta_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *log_meta[1];

	FILE *test_log = fmemopen((void*)log_meta, 1, "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -EIO);
}

/*
 * Test failure to read log metadata for child
 */
void test_cbt_util_coalesce_no_child_meta_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *parent_meta;
	void *child_meta[1];

	parent_meta = malloc(sizeof(struct cbt_log_metadata));
	FILE *parent_log = fmemopen((void*)parent_meta,
								sizeof(struct cbt_log_metadata), "r");
	FILE *child_log = fmemopen((void*)child_meta, 1, "r");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -EIO);

	free(parent_meta);
}

/*
 * Test failure when parent bitmap is larger than child bitmap
 */
void test_cbt_util_coalesce_larger_parent_bitmap_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *parent_meta;
	void *child_meta;

	parent_meta = malloc(sizeof(struct cbt_log_metadata));
	child_meta  = malloc(sizeof(struct cbt_log_metadata));

	((struct cbt_log_metadata *)parent_meta)->size = 1310720;
	((struct cbt_log_metadata *)child_meta)->size = 655360;

	FILE *parent_log = fmemopen((void*)parent_meta,
								sizeof(struct cbt_log_metadata), "r");
	FILE *child_log = fmemopen((void*)child_meta,
								sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -EINVAL);

	free(parent_meta);
	free(child_meta);
}

/*
 * Test failure to allocate bitmap buffer for parent
 */
void test_cbt_util_coalesce_parent_bitmap_malloc_failure(void **state)
{
	int result;
	int file_size;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *log_meta;

	file_size = 4194304 + sizeof(struct cbt_log_metadata);
	log_meta = malloc(file_size);
	FILE *parent_log = fmemopen((void*)log_meta, file_size, "r");
	FILE *child_log = fmemopen((void*)log_meta, file_size, "r+");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	malloc_succeeds(true);
	malloc_succeeds(true);
	malloc_succeeds(false);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
	free(log_meta);
}

/*
 * Test failure to allocate bitmap buffer for child
 */
void test_cbt_util_coalesce_child_bitmap_malloc_failure(void **state)
{
	int result;
	uint64_t disk_size, file_size;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *log_meta;

  	disk_size = 2199023255552; //2TB
	file_size = bitmap_size(disk_size) + sizeof(struct cbt_log_metadata);
	log_meta = malloc(file_size);
	((struct cbt_log_metadata*)log_meta)->size = disk_size;
	FILE *parent_log = fmemopen((void*)log_meta, file_size, "r");
	FILE *child_log = fmemopen((void*)log_meta, file_size, "r+");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	malloc_succeeds(true);
	malloc_succeeds(true);
	malloc_succeeds(true);
	malloc_succeeds(false);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -ENOMEM);

	disable_malloc_mock();
	free(log_meta);
}

/*
 * Test failure to read bitmap from parent log file
 */
void test_cbt_util_coalesce_parent_no_bitmap_data_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *log_meta;

	log_meta = malloc(sizeof(struct cbt_log_metadata));
	//Intialise size in metadata file
	((struct cbt_log_metadata*)log_meta)->size = 2199023255552; //2TB
	FILE *parent_log = fmemopen((void*)log_meta,
							sizeof(struct cbt_log_metadata), "r");
	FILE *child_log = fmemopen((void*)log_meta,
							sizeof(struct cbt_log_metadata), "r+");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -EIO);

	free(log_meta);
}

/*
 * Test failure to read bitmap from child log file
 */
void test_cbt_util_coalesce_child_no_bitmap_data_failure(void **state)
{
	int result;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *parent_meta;
	void *child_meta;
	uint64_t disk_size, file_size;

	disk_size = 2199023255552; //2TB
	file_size = bitmap_size(disk_size) + sizeof(struct cbt_log_metadata);

	parent_meta = malloc(file_size);
	child_meta = malloc(sizeof(struct cbt_log_metadata));
	//Intialise size in metadata file
	((struct cbt_log_metadata*)parent_meta)->size = disk_size; 
	((struct cbt_log_metadata*)child_meta)->size = disk_size;
	FILE *parent_log = fmemopen((void*)parent_meta, file_size, "r");
	FILE *child_log = fmemopen((void*)child_meta,
							sizeof(struct cbt_log_metadata), "r+");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -EIO);

	free(parent_meta);
	free(child_meta);
}

/*
 * Test successful coalesce
 */
void test_cbt_util_coalesce_success(void **state)
{
	int result;
	int file_size;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *parent_data;
	void *child_data;
	struct fwrite_data *output;
	uint64_t size = 4194304;

	uint64_t bmsize = bitmap_size(size);
	file_size = sizeof(struct cbt_log_metadata) + bmsize;
	parent_data = malloc(file_size);
	child_data = malloc(file_size);

	//Intialise size in metadata file
	((struct cbt_log_metadata*)parent_data)->size = size;
	//Fill bitmap with random bytes
	memcpy(parent_data + sizeof(struct cbt_log_metadata), (void*)memcpy, bmsize );
	FILE *parent_log = fmemopen((void*)parent_data, file_size, "r");

	//Intialise size in metadata file
	((struct cbt_log_metadata*)child_data)->size = size;
	//Fill bitmap with random bytes
	memcpy(child_data + sizeof(struct cbt_log_metadata), (void*)memcpy, bmsize );
	FILE *child_log = fmemopen((void*)child_data, file_size, "r");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);
	enable_mock_fwrite();
	output = setup_fwrite_mock(bmsize);

	result = cbt_util_coalesce(6, args);
	// OR the contents of bitmap of both files
	*((char *)child_data + sizeof(struct cbt_log_metadata))
				|= *((char *)parent_data + sizeof(struct cbt_log_metadata));
	assert_int_equal(result, 0);
	assert_memory_equal(output->buf,
				child_data + sizeof(struct cbt_log_metadata), bmsize);

	free_fwrite_data(output);
	free(parent_data);
	free(child_data);
}

/*
 * Test failure to set file pointer to start of bitmap area
 */
void test_cbt_util_coalesce_set_file_pointer_failure(void **state)
{
	int result;
	int file_size;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *parent_data;
	void *child_data;
	uint64_t size = 4194304;

	uint64_t bmsize = bitmap_size(size);
	file_size = sizeof(struct cbt_log_metadata) + bmsize;
	parent_data = malloc(file_size);
	child_data = malloc(file_size);

	//Intialise size in metadata file
	((struct cbt_log_metadata*)parent_data)->size = size;
	//Fill bitmap with random bytes
	memcpy(parent_data + sizeof(struct cbt_log_metadata), (void*)memcpy, bmsize );
	FILE *parent_log = fmemopen((void*)parent_data, file_size, "r");

	//Intialise size in metadata file
	((struct cbt_log_metadata*)child_data)->size = size;
	//Fill bitmap with random bytes
	memcpy(child_data + sizeof(struct cbt_log_metadata), (void*)memcpy, bmsize );
	FILE *child_log = fmemopen((void*)child_data, file_size, "r+");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	fail_fseek(EIO);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -EIO);

	free(parent_data);
	free(child_data);
}

/*
 * Test failure to write bitmap to log file
 */
void test_cbt_util_coalesce_write_bitmap_failure(void **state)
{
	int result;
	int file_size;
	char* args[] = { "cbt-util", "coalesce", "-p", "test_parent.log", "-c" , "test_child.log"};
	void *parent_data;
	void *child_data;
	uint64_t size = 4194304;

	uint64_t bmsize = bitmap_size(size);
	file_size = sizeof(struct cbt_log_metadata) + bmsize;
	parent_data = malloc(file_size);
	child_data = malloc(file_size);

	//Intialise size in metadata file
	((struct cbt_log_metadata*)parent_data)->size = size;
	//Fill bitmap with random bytes
	memcpy(parent_data + sizeof(struct cbt_log_metadata), (void*)memcpy, bmsize );
	FILE *parent_log = fmemopen((void*)parent_data, file_size, "r");

	//Intialise size in metadata file
	((struct cbt_log_metadata*)child_data)->size = size;
	//Fill bitmap with random bytes
	memcpy(child_data + sizeof(struct cbt_log_metadata), (void*)memcpy, bmsize );
	FILE *child_log = fmemopen((void*)child_data, file_size, "r");

	will_return(__wrap_fopen, parent_log);
	expect_value(__wrap_fclose, fp, parent_log);
	will_return(__wrap_fopen, child_log);
	expect_value(__wrap_fclose, fp, child_log);

	result = cbt_util_coalesce(6, args);
	assert_int_equal(result, -EIO);

	free(parent_data);
	free(child_data);
}
