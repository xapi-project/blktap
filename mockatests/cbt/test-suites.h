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

#ifndef __TEST_SUITES_H__
#define __TEST_SUITES_H__

#include <setjmp.h>
#include <cmocka.h>
#include <uuid/uuid.h>

struct cbt_log_metadata {
	uuid_t parent;
	uuid_t child;
	int    consistent;
};

/* Command lookup tests */
void test_get_command_create(void **state);
void test_get_command_set(void **state);
void test_get_command_get(void **state);
void test_get_command_bad_command(void **state);
void test_get_command_over_long_command(void **state);

void test_help_success(void ** state);

/* 'cbt-util get' tests */
void test_cbt_util_get_flag(void **state);
void test_cbt_util_get_parent(void **state);
void test_cbt_util_get_child(void **state);
void test_cbt_util_get_nofile_failure(void **state);
void test_cbt_util_get_nodata_failure(void **state);
void test_cbt_util_get_malloc_failure(void **state);
void test_cbt_util_get_no_name_failure(void **state);
void test_cbt_util_get_no_command_failure(void **state);

/* 'cbt-util create' tests */
void test_cbt_util_create_success(void **state);
void test_cbt_util_create_file_open_failure(void **state);
void test_cbt_util_create_metadata_write_failure(void **state);
void test_cbt_util_create_bitmap_write_failure(void **state);
void test_cbt_util_create_log_data_allocation_failure(void **state);
void test_cbt_util_create_bitmap_allocation_failure(void **state);
void test_cbt_util_create_no_name_failure(void **state);
void test_cbt_util_create_no_size_failure(void **state);

/* Functions under test */

extern int cbt_util_create(int , char **);
extern int cbt_util_set(int , char **);
extern int cbt_util_get(int , char **);
extern void help(void);

static const struct CMUnitTest cbt_command_tests[] = {
	cmocka_unit_test(test_get_command_create),
	cmocka_unit_test(test_get_command_set),
	cmocka_unit_test(test_get_command_get),
	cmocka_unit_test(test_get_command_bad_command),
	cmocka_unit_test(test_get_command_over_long_command),
	cmocka_unit_test(test_help_success)
};

static const struct CMUnitTest cbt_get_tests[] = {
	cmocka_unit_test(test_cbt_util_get_flag),
	cmocka_unit_test(test_cbt_util_get_parent),
	cmocka_unit_test(test_cbt_util_get_child),
	cmocka_unit_test(test_cbt_util_get_nofile_failure),
	cmocka_unit_test(test_cbt_util_get_nodata_failure),
	cmocka_unit_test(test_cbt_util_get_malloc_failure),
	cmocka_unit_test(test_cbt_util_get_no_name_failure),
	cmocka_unit_test(test_cbt_util_get_no_command_failure)
};

static const struct CMUnitTest cbt_create_tests[] = {
	cmocka_unit_test(test_cbt_util_create_success),
	cmocka_unit_test(test_cbt_util_create_file_open_failure),
	cmocka_unit_test(test_cbt_util_create_metadata_write_failure),
	cmocka_unit_test(test_cbt_util_create_bitmap_write_failure),
	cmocka_unit_test(test_cbt_util_create_log_data_allocation_failure),
	cmocka_unit_test(test_cbt_util_create_bitmap_allocation_failure),
	cmocka_unit_test(test_cbt_util_create_no_name_failure),
	cmocka_unit_test(test_cbt_util_create_no_size_failure)
};

#endif /* __TEST_SUITES_H__ */
