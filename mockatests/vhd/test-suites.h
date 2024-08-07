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

#ifndef __TEST_SUITES_H__
#define __TEST_SUITES_H__

#include <setjmp.h>
#include <cmocka.h>
#include <uuid/uuid.h>
#include <stdint.h>
#include <vhd-util.h>
#include "vhd-wrappers.h"

/* 'vhd-util snapshot' tests */
void test_vhd_util_snapshot_enospc_from_vhd_snapshot(void **state);
void test_vhd_util_snapshot_einval_from_vhd_open(void **state);
void test_vhd_util_snapshot_einval_from_vhd_get_keyhash(void **state);
void test_vhd_util_snapshot_einval_from_vhd_open_cookie(void **state);
void test_vhd_util_snapshot_einval_from_vhd_set_keyhash(void **state);

/* Functions under test */
extern int vhd_util_snapshot(int , char **);

static int setup(void **state) {
     reset_flags();
     return 0;
}

static const struct CMUnitTest vhd_snapshot_tests[] = {
	cmocka_unit_test(test_vhd_util_snapshot_enospc_from_vhd_snapshot),
	cmocka_unit_test(test_vhd_util_snapshot_einval_from_vhd_open),
	cmocka_unit_test(test_vhd_util_snapshot_einval_from_vhd_get_keyhash),
	cmocka_unit_test_setup(test_vhd_util_snapshot_einval_from_vhd_open_cookie, setup),
	cmocka_unit_test_setup(test_vhd_util_snapshot_einval_from_vhd_set_keyhash, setup)
};

/* 'canonpath' tests */
void test_vhd_util_canon_path_relative_success(void **state);

static const struct CMUnitTest canonpath_tests[] = {
	cmocka_unit_test(test_vhd_util_canon_path_relative_success)
};

/* Utility function test */
void test_vhd_validate_header_bad_cookie(void **state);
void test_vhd_validate_header_bad_version(void **state);
void test_vhd_validate_header_bad_offset(void **state);
void test_vhd_validate_header_bad_blocksize(void **state);
void test_vhd_validate_header_bad_checksum(void **state);
void test_vhd_validate_header_success(void **state);

static const struct CMUnitTest utility_tests[] = {
	cmocka_unit_test(test_vhd_validate_header_bad_cookie),
	cmocka_unit_test(test_vhd_validate_header_bad_version),
	cmocka_unit_test(test_vhd_validate_header_bad_offset),
	cmocka_unit_test(test_vhd_validate_header_bad_blocksize),
	cmocka_unit_test(test_vhd_validate_header_bad_checksum),
	cmocka_unit_test(test_vhd_validate_header_success)
};

/* Bit ops tests */
void test_set_clear_test_bit(void **state);
void test_bitmaps(void **state);

static const struct CMUnitTest bitops_tests[] = {
	cmocka_unit_test(test_set_clear_test_bit),
	cmocka_unit_test(test_bitmaps)
};

#endif /* __TEST_SUITES_H__ */
