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

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>

#include <wrappers.h>

static int tests_running = 1;
static int mock_malloc = 0;
static int mock_fwrite = 0;
static int mock_vprintf = 0;
static int mock_fseek = 0;

void *
__wrap_malloc(size_t size)
{
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

void
__wrap_free(void *ptr)
{
	/*fprintf(stderr, "Freeing block at %p\n", ptr);*/
	test_free(ptr);
}

FILE *
__wrap_fopen(void)
{
	FILE *file = (FILE*)mock();
	if (file == NULL) {
		errno = ENOENT;
	}

	return file;
}

void __real_fclose(FILE *fp);

void
__wrap_fclose(FILE *fp)
{
	if (tests_running) {
		check_expected_ptr(fp);
	}
	__real_fclose(fp);
}

int
__real_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int
__wrap_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (mock_fwrite) {
		struct fwrite_data *data = mock_ptr_type(struct fwrite_data *);

		size_t remaining = data->size - data->offset;
		size_t len = size * nmemb;

		assert_in_range(len, 0, remaining);
		memcpy(data->buf + data->offset, ptr, len);

		data->offset += len;

		return len;
	}
	return __real_fwrite(ptr, size, nmemb, stream);
}

struct fwrite_data *setup_fwrite_mock(int size)
{
	void *buf;
	struct fwrite_data *data;

	buf = test_malloc(size);
	memset(buf, 0, size);

	data = test_malloc(sizeof(struct fwrite_data));
	data->size = size;
	data->offset = 0;
	data->buf = buf;
	data->type = stdout;

	will_return_always(__wrap_fwrite, data);

	return data;
}

void free_fwrite_data(struct fwrite_data *data)
{
	if (data->buf)
		test_free(data->buf);
	test_free(data);
	mock_fwrite = false;
}

int
wrap_vprintf(const char *format, va_list ap)
{
	if (mock_vprintf) {
		struct printf_data *data = (struct printf_data *)mock();
		int remaining = data->size - data->offset;
		int len = vsnprintf(data->buf + data->offset, remaining, format, ap);
		assert_in_range(len, 0, remaining);
		data->offset += len;
		return len;
	} else {
		fprintf(stderr, "Unexpected call to printf\n");
		vfprintf(stderr, format, ap);
	}
	return 0;
}

int
__wrap_printf(const char *format, ...)
{
	int ret;
	va_list ap;
	va_start(ap, format);

	ret = wrap_vprintf(format, ap);

	va_end(ap);

	return ret;
}

int
__wrap___printf_chk (int __flag, const char *format, ...)
{
	int ret;
	va_list ap;
	va_start(ap, format);

	ret =  wrap_vprintf(format, ap);

	va_end(ap);

	return ret;
}

int
__wrap_puts(const char *s)
{
	return __wrap_printf("%s\n", s);
}

struct printf_data *setup_vprintf_mock(int size)
{
	char *buf;
	struct printf_data *data;

	buf = test_malloc(size);
	memset(buf, 0, size);

	data = test_malloc(sizeof(struct printf_data));
	data->size = size;
	data->offset = 0;
	data->buf = buf;

	will_return_always(wrap_vprintf, data);

	mock_vprintf = 1;

	return data;
}

void free_printf_data(struct printf_data *data)
{
	if (data->buf)
		test_free(data->buf);
	test_free(data);
	mock_vprintf = 0;
}

int __real_fseek(FILE *stream, long offset, int whence);

int __wrap_fseek(FILE *stream, long offset, int whence)
{
	if(mock_fseek){
		mock_fseek = 0;
		errno = mock();
		return -1;
	}

	return __real_fseek(stream, offset, whence);
}

void fail_fseek(int Errno)
{
	will_return(__wrap_fseek, Errno);
	mock_fseek = 1;
}


void malloc_succeeds(bool succeed)
{
	mock_malloc = true;
	will_return(__wrap_malloc, succeed);
}

void disable_malloc_mock()
{
	mock_malloc = false;
}

void disable_mocks()
{
	tests_running = 0;
}

void enable_mock_fwrite()
{
	mock_fwrite = true;
}
