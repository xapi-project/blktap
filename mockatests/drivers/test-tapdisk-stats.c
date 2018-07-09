/*
 * Copyright (c) 2018, Citrix Systems, Inc.
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
#include <stdlib.h>

#include "test-suites.h"

#include "tapdisk-stats.h"

#define TD_CTL_TEST_BUFSIZ ((size_t)64)

/* Test that filling a normal stats buffer produces the
 * expected output */
void
test_stats_normal_buffer(void **state)
{
	td_stats_t 	st;
	char		*buf;

	buf = malloc(TD_CTL_TEST_BUFSIZ);
	assert_non_null(buf);

	tapdisk_stats_init(&st, buf, TD_CTL_TEST_BUFSIZ);

	tapdisk_stats_enter(&st, '{');
	tapdisk_stats_val(&st, "d", 123456789);
	tapdisk_stats_leave(&st, '}');

	assert_string_equal((char*)st.buf, "{ 123456789 }");

	/* tapdisk_stats_init() put buf into st->buf and in the general case
	 * it might have been realloc()'d so don't free buf here */
	free(st.buf);
}

/* Test that filling the stats buffer with more than the 
 * initial allocation correctly reallocates and extends
 * the buffer
 */
void
test_stats_realloc_buffer(void **state)
{
	td_stats_t 	st;
	char		*buf;
	int			i;

	buf = malloc(TD_CTL_TEST_BUFSIZ);
	assert_non_null(buf);

	tapdisk_stats_init(&st, buf, TD_CTL_TEST_BUFSIZ);

	for (i = 0; i < 10; i++) {
		tapdisk_stats_val(&st, "d", 1234567890);
	}

	assert_string_equal((char*)st.buf, "1234567890, 1234567890, 1234567890, "
		"1234567890, 1234567890, 1234567890, 1234567890, 1234567890, "
		"1234567890, 1234567890");

	/* tapdisk_stats_init() put buf into st->buf and in the general case
	 * it might have been realloc()'d so don't free buf here */
	free(st.buf);
}

/* Test that filling the stats buffer to exactly the
 * initial buffer length not including the terminating
 * NULL is handled correctly. Note if we change TD_CTL_TEST_BUFSIZ
 * then this test has to change.
 */
void
test_stats_realloc_buffer_edgecase(void **state)
{
	td_stats_t 	st;
	char		*buf;
	int			i;

	buf = malloc(TD_CTL_TEST_BUFSIZ);
	assert_non_null(buf);

	tapdisk_stats_init(&st, buf, TD_CTL_TEST_BUFSIZ);

	/* Put in 8 characters initially */
	tapdisk_stats_val(&st, "d", 12345678);
	/* Then 7 lots of 6 because the comma-space will pad it to 8 */
	for (i = 0; i < 7; i++) {
		tapdisk_stats_val(&st, "d", 123456);
	}
	assert_string_equal((char*)st.buf, "12345678, 123456, 123456, "
		"123456, 123456, 123456, 123456, 123456");

	/* tapdisk_stats_init() put buf into st->buf and in the general case
	 * it might have been realloc()'d so don't free buf here */
	free(st.buf);
}
