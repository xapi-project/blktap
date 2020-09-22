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
#include <limits.h>

#include "test-suites.h"
#include "libvhd.h"
#include "vhd-wrappers.h"

char *
__real_canonpath(const char *path, char *resolved_path);

void test_vhd_util_canon_path_relative_success(void **state)
{
	char *ppath, __ppath[PATH_MAX];
	char *pname = "6f95a71c-4961-4af0-aec4-58bc3bbef254.vhd";
	char *test_dir = "/run/sr-mount/test";
	char *test_path = "/run/sr-mount/test/6f95a71c-4961-4af0-aec4-58bc3bbef254.vhd";

	char *dir_name_return = test_malloc(strlen(test_dir) + 1);
	strncpy(dir_name_return, test_dir, strlen(test_dir) + 1);

	char *realpath_return = test_malloc(strlen(test_path) + 1);
	strncpy(realpath_return, test_path, strlen(test_path) + 1);

	will_return(__wrap_get_current_dir_name, dir_name_return);
	will_return(__wrap_realpath, realpath_return);

	/* Have to call __real_canonpath here to bypass the wrapping
	   done for testing the snapshot commands.
	 */
	ppath = __real_canonpath(pname, __ppath);

	fprintf(stderr, "ppath %s\n", ppath);

	assert_non_null(ppath);
	test_free(ppath);
}
