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
#include <sys/file.h>

#include <wrappers.h>
#include "control-wrappers.h"
#include "test-suites.h"

#include "tap-ctl.h"
#include "blktap.h"

void test_tap_ctl_free_open_fail(void **state)
{
	int dev_fd = -1;
	int result;

	will_return(__wrap_open, dev_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk");

	result = tap_ctl_free(0);

	assert_int_equal(result, -ENOENT);
}

void test_tap_ctl_free_success(void **state)
{
	int dev_fd = 12;
	int marker_fd = 13;
	int result;

	will_return(__wrap_open, dev_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, dev_fd);
	expect_value(__wrap_flock, operation, LOCK_EX);

	/* Open and lock, non-blocking, the marker file */
	will_return(__wrap_open, marker_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, marker_fd);
	expect_value(__wrap_flock, operation, LOCK_EX | LOCK_NB);

	will_return(__wrap_unlink, 0);
	expect_string(__wrap_unlink, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, marker_fd);
	expect_value(__wrap_flock, operation, LOCK_UN);

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, marker_fd);

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, dev_fd);
	expect_value(__wrap_flock, operation, LOCK_UN);

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, dev_fd);

	result = tap_ctl_free(0);

	assert_int_equal(result, 0);
}

void test_tap_ctl_free_locked(void **state)
{
	int dev_fd = 12;
	int marker_fd = 13;
	int result;

	will_return(__wrap_open, dev_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, dev_fd);
	expect_value(__wrap_flock, operation, LOCK_EX);

	/* Open and lock, non-blocking, the marker file */
	will_return(__wrap_open, marker_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_flock, EAGAIN);
	expect_value(__wrap_flock, fd, marker_fd);
	expect_value(__wrap_flock, operation, LOCK_EX | LOCK_NB);

	will_return(__wrap_flock, marker_fd);
	expect_value(__wrap_flock, fd, marker_fd);
	expect_value(__wrap_flock, operation, LOCK_UN);

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, marker_fd);

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, dev_fd);
	expect_value(__wrap_flock, operation, LOCK_UN);

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, dev_fd);

	result = tap_ctl_free(0);

	assert_int_equal(result, -EAGAIN);
}

