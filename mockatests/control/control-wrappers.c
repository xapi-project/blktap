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

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>

#include "control-wrappers.h"

static int mock_fdopen = 0;
static int mock_open = 0;


FILE *
__real_fdopen(int fd, const char *mode);

FILE *
__wrap_fdopen(int fd, const char *mode)
{
	if (mock_fdopen) {
		FILE *file = (FILE*)mock();
		if (file == NULL) {
			errno = ENOENT;
		}

		return file;
	}

	return __real_fdopen(fd, mode);
}

/*
 * Enable the wrapping function for fdopen
 */
void enable_mock_fdopen()
{
	mock_fdopen = 1;
}

int
__wrap_ioctl(int fd, int request, ...)
{
	int result;

	check_expected(fd);
	check_expected(request);

	result = (int)mock();

	if (result != 0) {
		errno = result;
		result = -1;
	}

	return result;
}

int
__real_open(const char *pathname, int flags);

int
__wrap_open(const char *pathname, int flags)
{
	int result;

	if (mock_open) {
		result = mock();
		if (result == -1)
			errno = ENOENT;
		return result;
	}

	return __real_open(pathname, flags);
}

int
__real_close(int fd);

int
__wrap_close(int fd)
{
	int result;

	if (mock_open) {
		check_expected(fd);
		result = mock();
		if (result != 0)
		{
			errno = result;
			result = 1;
		}
		return result;
	}

	return __real_close(fd);
}

/*
 * Enable the wrapping of open
 */
void enable_mock_open()
{
	mock_open = true;
}

void disable_control_mocks()
{
	mock_open = 0;
	mock_fdopen = 0;
}
