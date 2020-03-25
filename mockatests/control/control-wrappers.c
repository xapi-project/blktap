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
#include <sys/socket.h>

#include "control-wrappers.h"

static int enable_mocks = 0;


FILE *
__real_fdopen(int fd, const char *mode);

FILE *
__wrap_fdopen(int fd, const char *mode)
{
	if (enable_mocks) {
		FILE *file = (FILE*)mock();
		if (file == NULL) {
			errno = ENOENT;
		}

		return file;
	}

	return __real_fdopen(fd, mode);
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

	if (enable_mocks) {
		check_expected(pathname);
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

	if (enable_mocks) {
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

int
__real_access(const char *pathname, int mode);

int
__wrap_access(const char *pathname, int mode)
{
	int result;

	if (enable_mocks) {
		check_expected(pathname);

		result = mock();
		if (result != 0) {
			errno = result;
			result = -1;
		}
		return result;
	}
	return __real_access(pathname, mode);
}

size_t
__real_read(int fd, void *buf, size_t count);

size_t
__wrap_read(int fd, void *buf, size_t count)
{
	fprintf(stderr, "__wrap_read\n");

	if (enable_mocks) {
		return -1;
	}

	return __real_read(fd, buf, count);
}

size_t
__real_write(int fd, const void *buf, size_t count);

size_t
__wrap_write(int fd, const void *buf, size_t count)
{
	fprintf(stderr, "__wrap_write\n");
	if (enable_mocks) {
		return -1;
	}

	return __real_write(fd, buf, count);
}

int
__real_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

int
__wrap_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	fprintf(stderr, "__wrap_connect\n");
	if (enable_mocks) {
		return -1;
	}
	return __real_connect(sockfd, addr, addrlen);
}

int
__real_select(int nfds, fd_set *readfds, fd_set *writefds,
	      fd_set *exceptfds, struct timeval *timeout);

int
__wrap_select(int nfds, fd_set *readfds, fd_set *writefds,
	      fd_set *exceptfds, struct timeval *timeout)
{
	if (enable_mocks) {
		return -1;
	}

	return __real_select(nfds, readfds, writefds, exceptfds, timeout);
}

int
__real_mkdir(const char *pathname, mode_t mode);

int
__wrap_mkdir(const char *pathname, mode_t mode)
{
	int result;
	if (enable_mocks) {
		check_expected(pathname);
		result = mock();
		if (result != 0){
			errno = result;
			result = -1;
		}
		return result;
	}
	return __real_mkdir(pathname, mode);
}

int
__real_flock(int fd, int operation);

int
__wrap_flock(int fd, int operation)
{
	int result;
	if (enable_mocks) {
		check_expected(fd);
		result = mock();
		if (result != 0) {
			errno = result;
			result = -1;
		}
		return result;
	}
	return __real_flock(fd, operation);
}

int
__real___xmknod(int ver, const char * path, mode_t mode, dev_t * dev);

int
__wrap___xmknod(int ver, const char *pathname, mode_t mode, dev_t * dev)
{
	int result;
	if (enable_mocks)
	{
		check_expected(pathname);
		result = mock();
		if (result != 0)
		{
			errno = result;
			result = -1;
		}
		return result;
	}
	return __real___xmknod(ver, pathname, mode, dev);
}

int
__real_unlink(const char *pathname);

int
__wrap_unlink(const char *pathname)
{
	int result;
	if (enable_mocks)
	{
		check_expected(pathname);
		result = mock();
		if (result != 0)
		{
			errno = result;
			result = -1;
		}
		return result;
	}
	return __real_unlink(pathname);
}

void enable_control_mocks()
{
	enable_mocks = true;
}

void disable_control_mocks()
{
	enable_mocks = 0;
}
