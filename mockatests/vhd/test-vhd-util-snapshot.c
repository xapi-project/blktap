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
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <wrappers.h>

#include "test-suites.h"
#include "libvhd.h"
#include "vhd-wrappers.h"

#define RAW_PRT_ARGS_SIZE 9

char* raw_prt_args[RAW_PRT_ARGS_SIZE] = {
		"--debug",
		"-n",
		"test1.vhdcache",
		"-p",
		"test2.vhdcache",
		"-S",
		"71680",
		"-e",
		"-m"};

#define ARGS_SIZE 8

char* args[ARGS_SIZE] = {
		"--debug",
		"-n",
		"test1.vhdcache",
		"-p",
		"test2.vhdcache",
		"-S",
		"71680",
		"-e"};

/*
 * Tests to ensure errors are propagated by vhd-util-snapshot.
 */
void test_vhd_util_snapshot_enospc_from_vhd_snapshot(void **state)
{
	will_return(__wrap_canonpath, "testing");
	will_return(__wrap_vhd_snapshot, ENOSPC);
	expect_any(__wrap_free, in);
	int res = vhd_util_snapshot(RAW_PRT_ARGS_SIZE, raw_prt_args);
	assert_int_equal(get_close_count(), 0);
	assert_int_equal(res, ENOSPC);
}

void test_vhd_util_snapshot_einval_from_vhd_open(void **state)
{
	will_return(__wrap_canonpath, "testing");
	will_return(__wrap_vhd_snapshot, 0);
	will_return(__wrap_vhd_open, EINVAL);
	expect_any(__wrap_free, in);
	int res = vhd_util_snapshot(ARGS_SIZE, args);
	assert_int_equal(get_close_count(), 0);
	assert_int_equal(res, EINVAL);
}

void test_vhd_util_snapshot_einval_from_vhd_get_keyhash(void **state)
{
	will_return(__wrap_canonpath, "testing");
	will_return(__wrap_vhd_snapshot, 0);
	will_return(__wrap_vhd_open, 0);
	will_return(__wrap_vhd_get_keyhash, EINVAL);
	will_return(__wrap_vhd_close, 0);
	expect_any(__wrap_vhd_close, ctx);
	expect_any(__wrap_free, in);
	int res = vhd_util_snapshot(ARGS_SIZE, args);
	assert_int_equal(get_close_count(), 1);
	assert_int_equal(res, EINVAL);
}

void test_vhd_util_snapshot_einval_from_vhd_open_cookie(void **state)
{
	will_return(__wrap_canonpath, "testing");
	will_return(__wrap_vhd_snapshot, 0);
	set_cookie();
	will_return(__wrap_vhd_get_keyhash, 0);
	will_return(__wrap_vhd_close, 0);
	expect_any(__wrap_vhd_close, ctx);
	int err[] = {0, EINVAL};
	set_open_errors(2, err);
	expect_any(__wrap_free, in);
	int res = vhd_util_snapshot(ARGS_SIZE, args);
	assert_int_equal(get_close_count(), 1);
	assert_int_equal(res, EINVAL);
}

void test_vhd_util_snapshot_einval_from_vhd_set_keyhash(void **state)
{
	will_return(__wrap_canonpath, "testing");
	will_return(__wrap_vhd_snapshot, 0);
	will_return_always(__wrap_vhd_open, 0);
	set_cookie();
	will_return(__wrap_vhd_get_keyhash, 0);
	will_return_always(__wrap_vhd_close, 0);
	will_return(__wrap_vhd_set_keyhash, EINVAL);
	expect_any_count(__wrap_vhd_close, ctx, 2);
	expect_any(__wrap_free, in);
	int res = vhd_util_snapshot(ARGS_SIZE, args);
	assert_int_equal(get_close_count(), 2);
	assert_int_equal(res, EINVAL);
}
