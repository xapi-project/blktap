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

#ifndef __WRAPPERS_H__
#define __WRAPPERS_H__

#include <stdio.h>
#include <stdbool.h>

struct printf_data {
	int size;
	int offset;
	char *buf;
};

struct fwrite_data {
	FILE *type;
	int size;
	int offset;
	char *buf;
};

FILE * __wrap_fopen(void);

void __wrap_fclose(FILE *fp);

struct fwrite_data *setup_fwrite_mock(int size);

struct printf_data *setup_vprintf_mock(int size);

void free_printf_data(struct printf_data *data);

void free_fwrite_data(struct fwrite_data *data);

/*
 * This enables mocking of malloc and provides a flag to control
 * whether malloc succeeds. Mocking stays in force until a call to
 * disable_malloc_mock.
 *
 * Subsequent calls to mallloc_suceeds will queue successive results
 * for the mock.
 */
void malloc_succeeds(bool succeed);

void disable_malloc_mock();

void disable_mocks();

void enable_mock_fwrite();

int __wrap_printf(const char *format, ...);

int __wrap___printf_chk (int __flag, const char *format, ...);

int __wrap_puts(const char *s);

#endif /* __WRAPPERS_H__ */
