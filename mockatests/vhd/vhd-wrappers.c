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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "libvhd.h"
#include "vhd-wrappers.h"

static int	cookie = 0;
static int	close_count = 0;
static int	*open_return_errs = NULL;
static int	n_return_errs = 0;
static int	open_call_count = 0;

static bool     mock_malloc = false;
static bool     use_real_allocator = false;

void reset_flags()
{
	close_count = 0;
	cookie = 0;
	open_return_errs = NULL;
	n_return_errs = 0;
	open_call_count = 0;
}

int get_close_count()
{
	return close_count;
}

void set_open_errors(int nerrs, int* errs)
{
	open_return_errs = errs;
	n_return_errs = nerrs;
}

void set_cookie()
{
	cookie = 1;
}

char *__wrap_canonpath(const char *path, char *resolved_path)
{
	return (char*)mock();
}

int __wrap_vhd_snapshot(const char *snapshot, uint64_t bytes, const char *parent,
			uint64_t mbytes, uint32_t flag)
{
	return (int)mock();
}

int __wrap_vhd_open(vhd_context_t *ctx, const char *file, int flags)
{
	int ret;
	if(open_return_errs) {
		ret = open_return_errs[open_call_count];

	} else {
		ret = (int)mock();
	}
	open_call_count++;
	return ret;
}

int __wrap_vhd_get_keyhash(vhd_context_t *ctx, struct vhd_keyhash* hash)
{
	if(cookie) {
		hash->cookie = 1;
	}
	return (int)mock();
}

int __wrap_vhd_close(vhd_context_t *ctx)
{
	check_expected(ctx);
	close_count++;
	return (int)mock();
}

int __wrap_vhd_set_keyhash(vhd_context_t *ctx, struct vhd_keyhash* hash)
{
	return (int)mock();
}

void *
__real_malloc(size_t size);

void *
__wrap_malloc(size_t size)
{
	if (!use_real_allocator) {
		bool succeed = true;
		if (mock_malloc) {
			succeed = (bool) mock();
		}
		if (succeed) {
			void * result = test_malloc(size);
			/*fprintf(stderr, "Allocated block of %zu bytes at %p\n", size, result);*/
			return result;
		}
		return NULL;
	}
	return __real_malloc(size);
}

void *
__wrap_realloc(void *ptr, size_t size)
{
	bool succeed = true;
	if (mock_malloc) {
		succeed = (bool) mock();
	}
	if (succeed) {
		void * result = test_realloc(ptr, size);
		/*fprintf(stderr, "Reallocated %p to %zu bytes at %p\n", ptr, size, result);*/
		return result;
	}
	return NULL;
}


void __real_free(void *ptr);

void
__wrap_free(void *ptr)
{
	if (!use_real_allocator) {
		/*fprintf(stderr, "Freeing block at %p\n", ptr);*/
		test_free(ptr);
	} else {
		check_expected(ptr);
		__real_free(ptr);
	}
}

char *__wrap_get_current_dir_name(void)
{
	return (char *)mock();
}

char *__wrap_realpath(const char *path, char *resolved_path)
{
	return (char *)mock();
}


void set_use_real_allocator(bool val)
{
	use_real_allocator = val;
}

