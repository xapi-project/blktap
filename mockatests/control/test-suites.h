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
#include <stdint.h>
#include <stdlib.h>
#include <cbt-util.h>


/* tap-ctl allocate tests */
void test_tap_ctl_allocate_prep_dir_no_access(void **state);
void test_tap_ctl_allocate_no_device_info(void **state);
void test_tap_ctl_allocate_make_device_fail(void **state);
void test_tap_ctl_allocate_ring_create_fail(void **state);
void test_tap_ctl_allocate_io_device_fail(void **state);
void test_tap_ctl_allocate_success(void **state);

/* tap-ctl close tests */
void test_tap_ctl_close_success(void **state);
void test_tap_ctl_force_close_success(void **state);
void test_tap_ctl_close_connect_fail(void **state);
void test_tap_ctl_close_write_error(void **state);
void test_tap_ctl_close_read_error(void **state);
void test_tap_ctl_close_write_select_timeout(void **state);
void test_tap_ctl_close_read_select_timeout(void **state);
void test_tap_ctl_close_error_response(void **state);

/* tap-ctl free tests */
void test_tap_ctl_free_open_fail(void **state);
void test_tap_ctl_free_success(void **state);
void test_tap_ctl_free_ioctl_busy(void **state);

/* tap-ctl list tests */
void test_tap_ctl_list_success_no_results(void **state);
void test_tap_ctl_list_success_one_minor_no_td(void **state);
void test_tap_ctl_list_success_one_td_no_minor_no_path(void **state);
void test_tap_ctl_list_success_one_td_one_minor_no_path(void **state);
void test_tap_ctl_list_success(void **state);

static const struct CMUnitTest tap_ctl_allocate_tests[] = {
	cmocka_unit_test(test_tap_ctl_allocate_prep_dir_no_access)
	/* cmocka_unit_test(test_tap_ctl_allocate_no_device_info), */
	/* cmocka_unit_test(test_tap_ctl_allocate_make_device_fail), */
	/* cmocka_unit_test(test_tap_ctl_allocate_ring_create_fail), */
	/* cmocka_unit_test(test_tap_ctl_allocate_io_device_fail), */
	/* cmocka_unit_test(test_tap_ctl_allocate_success) */
};

static const struct CMUnitTest tap_ctl_close_tests[] = {
	cmocka_unit_test(test_tap_ctl_close_success),
	cmocka_unit_test(test_tap_ctl_force_close_success),
	cmocka_unit_test(test_tap_ctl_close_connect_fail),
	cmocka_unit_test(test_tap_ctl_close_write_error),
	cmocka_unit_test(test_tap_ctl_close_read_error),
	cmocka_unit_test(test_tap_ctl_close_write_select_timeout),
	cmocka_unit_test(test_tap_ctl_close_read_select_timeout),
	cmocka_unit_test(test_tap_ctl_close_error_response)
};

/* static const struct CMUnitTest tap_ctl_free_tests[] = { */
/* 	cmocka_unit_test(test_tap_ctl_free_open_fail), */
/* 	cmocka_unit_test(test_tap_ctl_free_success), */
/* 	cmocka_unit_test(test_tap_ctl_free_ioctl_busy) */
/* }; */

static const struct CMUnitTest tap_ctl_list_tests[] = {
	cmocka_unit_test(test_tap_ctl_list_success_no_results),
	cmocka_unit_test(test_tap_ctl_list_success_one_minor_no_td),
	cmocka_unit_test(test_tap_ctl_list_success_one_td_no_minor_no_path),
	cmocka_unit_test(test_tap_ctl_list_success_one_td_one_minor_no_path),
	cmocka_unit_test(test_tap_ctl_list_success)
};

#endif /* __TEST_SUITES_H__ */
