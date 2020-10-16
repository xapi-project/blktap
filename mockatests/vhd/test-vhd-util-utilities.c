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

int vhd_validate_header(vhd_header_t *header);

void set_platform_codes(vhd_header_t *header)
{
	int i;
	int n;

	n = sizeof(header->loc) / sizeof(vhd_parent_locator_t);
	for (i = 0; i < n; i++) {
		header->loc[i].code = PLAT_CODE_NONE;
	}
}

void test_vhd_validate_header_bad_cookie(void **state)
{
	vhd_header_t header;

	memset(header.cookie, 0, sizeof(header.cookie));

	header.checksum = vhd_checksum_header(&header);
	assert_int_equal(-EINVAL, vhd_validate_header(&header));
}


void test_vhd_validate_header_bad_version(void **state)
{
	vhd_header_t header;

	memcpy(header.cookie, DD_COOKIE, sizeof(header.cookie));

	header.hdr_ver = 0;

	header.checksum = vhd_checksum_header(&header);
	assert_int_equal(-EINVAL, vhd_validate_header(&header));
}

void test_vhd_validate_header_bad_offset(void **state)
{
	vhd_header_t header;

	memcpy(header.cookie, DD_COOKIE, sizeof(header.cookie));
	header.hdr_ver      = DD_VERSION;
	header.data_offset  = 0;
	header.block_size   = VHD_BLOCK_SIZE;
	header.prt_ts       = 0;
	header.res1         = 0;
	header.checksum = vhd_checksum_header(&header);
	assert_int_equal(-EINVAL, vhd_validate_header(&header));
}

void test_vhd_validate_header_bad_blocksize(void **state)
{
	vhd_header_t header;

	memcpy(header.cookie, DD_COOKIE, sizeof(header.cookie));
	header.hdr_ver      = DD_VERSION;
	header.data_offset  = (uint64_t) -1;
	header.prt_ts       = 0;
	header.res1         = 0;

	set_platform_codes(&header);

	/* 0 block size */
	header.block_size = 0;
	header.checksum = vhd_checksum_header(&header);
	assert_int_equal(-EINVAL, vhd_validate_header(&header));

	/* Too big */
	header.block_size = (1ULL << 22);
	header.checksum = vhd_checksum_header(&header);
	assert_int_equal(-EINVAL, vhd_validate_header(&header));

	/* Not Power of 2 */
	header.block_size = (1ULL << 21) - 2;
	header.checksum = vhd_checksum_header(&header);
	assert_int_equal(-EINVAL, vhd_validate_header(&header));
}

void test_vhd_validate_header_bad_checksum(void **state)
{
	vhd_header_t header;

	memcpy(header.cookie, DD_COOKIE, sizeof(header.cookie));
	header.hdr_ver      = DD_VERSION;
	header.data_offset  = (uint64_t) -1;
	header.prt_ts       = 0;
	header.res1         = 0;

	set_platform_codes(&header);

	header.block_size = (1ULL << 21);
	header.checksum = vhd_checksum_header(&header) - 1;
	assert_int_equal(-EINVAL, vhd_validate_header(&header));
}

void test_vhd_validate_header_success(void **state)
{
	vhd_header_t header;

	memcpy(header.cookie, DD_COOKIE, sizeof(header.cookie));
	header.hdr_ver      = DD_VERSION;
	header.data_offset  = (uint64_t) -1;
	header.prt_ts       = 0;
	header.res1         = 0;

	set_platform_codes(&header);

	header.block_size = (1ULL << 21);
	header.checksum = vhd_checksum_header(&header);
	assert_int_equal(0, vhd_validate_header(&header));
}
