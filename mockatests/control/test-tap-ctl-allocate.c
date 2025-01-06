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
#include <string.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <wrappers.h>
#include "control-wrappers.h"
#include "test-suites.h"

#include "tap-ctl.h"
#include "blktap.h"


void test_tap_ctl_allocate_prep_dir_no_access(void **state)
{
	int result;
	int minor;
	char *devname;

	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, EACCES);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");

	result = tap_ctl_allocate(&minor, &devname);

	assert_int_equal(-EACCES, result);
}

void test_tap_ctl_allocate_prep_runtime_dir_no_access(void **state)
{
	int result;
	int minor;
	char *devname;

	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");


	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control/tapdisk");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");
	    will_return(__wrap_mkdir, EACCES);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control/tapdisk");

	result = tap_ctl_allocate(&minor, &devname);

	assert_int_equal(-EACCES, result);
}

void test_tap_ctl_allocate_first_success(void **state)
{
	int dev_fd = 12;
	int result;
	int minor;
	char *devname;

	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");


	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control/tapdisk");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");
	    will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_open, dev_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, dev_fd);
	expect_value(__wrap_flock, operation, LOCK_EX);

	will_return(__wrap_stat, -1);
	will_return(__wrap_stat, ENOENT);
	expect_string(__wrap_stat, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_open, 13);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, 13);

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, 12);
	expect_value(__wrap_flock, operation, LOCK_UN);

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, 12);

	result = tap_ctl_allocate(&minor, &devname);

	assert_int_equal(0, result);
}

void test_tap_ctl_allocate_create_failed(void **state)
{
	int dev_fd = 12;
	int result;
	int minor;
	char *devname;

	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");


	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control/tapdisk");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_open, dev_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, dev_fd);
	expect_value(__wrap_flock, operation, LOCK_EX);

	will_return(__wrap_stat, -1);
	will_return(__wrap_stat, ENOENT);
	expect_string(__wrap_stat, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_open, -1);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, 12);
	expect_value(__wrap_flock, operation, LOCK_UN);

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, 12);

	result = tap_ctl_allocate(&minor, &devname);

	assert_int_equal(-ENOENT, result);
}

void test_tap_ctl_allocate_one_exists_success(void **state)
{
	int dev_fd = 12;
	int result;
	int minor;
	char *devname;
	struct stat st_buf;

	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");


	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control/tapdisk");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");
	    will_return(__wrap_mkdir, 0);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_open, dev_fd);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk");

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, dev_fd);
	expect_value(__wrap_flock, operation, LOCK_EX);

	will_return(__wrap_stat, 0);
	will_return(__wrap_stat, &st_buf);
	expect_string(__wrap_stat, pathname, "/run/blktap-control/tapdisk/tapdisk-0");

	will_return(__wrap_stat, -1);
	will_return(__wrap_stat, ENOENT);
	expect_string(__wrap_stat, pathname, "/run/blktap-control/tapdisk/tapdisk-1");

	will_return(__wrap_open, 13);
	expect_string(__wrap_open, pathname, "/run/blktap-control/tapdisk/tapdisk-1");

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, 13);

	will_return(__wrap_flock, 0);
	expect_value(__wrap_flock, fd, 12);
	expect_value(__wrap_flock, operation, LOCK_UN);

	will_return(__wrap_close, 0);
	expect_value(__wrap_close, fd, 12);

	result = tap_ctl_allocate(&minor, &devname);

	assert_int_equal(0, result);
}
