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

/* 'vhd-util snapshot' tests */
void test_vhd_util_snapshot_enospc_from_vhd_snapshot(void **state);
void test_vhd_util_snapshot_einval_from_vhd_open(void **state);
void test_vhd_util_snapshot_einval_from_vhd_get_keyhash(void **state);
void test_vhd_util_snapshot_einval_from_vhd_open_cookie(void **state);
void test_vhd_util_snapshot_einval_from_vhd_set_keyhash(void **state);

/* Functions under test */
extern int vhd_util_snapshot(int , char **);

static const struct CMUnitTest vhd_snapshot_tests[] = {
	cmocka_unit_test(test_vhd_util_snapshot_enospc_from_vhd_snapshot),
	cmocka_unit_test(test_vhd_util_snapshot_einval_from_vhd_open),
	cmocka_unit_test(test_vhd_util_snapshot_einval_from_vhd_get_keyhash),
	cmocka_unit_test(test_vhd_util_snapshot_einval_from_vhd_open_cookie),
	cmocka_unit_test(test_vhd_util_snapshot_einval_from_vhd_set_keyhash)
};

#endif /* __TEST_SUITES_H__ */
