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

void __wrap_free(void* in)
{
	check_expected(in);
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
