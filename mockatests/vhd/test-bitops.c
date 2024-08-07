/*
 * Copyright (c) 2024, Cloud Software Group, Inc.
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
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <wrappers.h>

#include "test-suites.h"
#include "libvhd.h"


void test_set_clear_test_bit(void **state) {
    /* Representative bitmap of 512 bytes */
    uint32_t bit;
    uint8_t *map = malloc(512);

    /* Start zero'd */
    bzero(map, 512);

    for (bit = 0; bit < 4096; bit++) {
        /* Start unset */
        assert_false(test_bit(map, bit));

        /* Set it and check */
        set_bit(map, bit);
        assert_true(test_bit(map, bit));

        /* Clear it and check */
        clear_bit(map, bit);
        assert_false(test_bit(map, bit));
    }

    free(map);
}

void test_bitmaps(void **state) {
	uint8_t map[2] = { 0x80, 0x01 };

	assert_true(test_bit(map, 0));
	assert_false(test_bit(map, 1));

	assert_false(test_bit(map, 14));
	assert_true(test_bit(map, 15));

	set_bit(map, 3);
	assert_true(map[0] == 0x90);

	clear_bit(map, 0);
	assert_true(map[0] == 0x10);
}

