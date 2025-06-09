/*
 * Copyright (c) 2016, Citrix Systems, Inc.
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

#ifndef __TAPDISK_UTIL_H__
#define __TAPDISK_UTIL_H_

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define ARRAY_SIZE(_a) (sizeof(_a)/sizeof((_a)[0]))

/*
 * Strncpy variant that guarantees to terminate the string
 */
static inline char *
safe_strncpy(char *dest, const char *src, size_t n)
{
	char *pdest;
	pdest = strncpy(dest, src, n - 1);
	if (n > 0)
		dest[n - 1] = '\0';
	return pdest;
}

/*
 * Constants for cryptographic operations
 */
#define MAX_AES_XTS_PLAIN_KEYSIZE 1024

/*
 * Base64 encoding/decoding utilities using OpenSSL
 */
int base64_encode_data(const uint8_t *input, size_t input_len, char **output);
int base64_decode_key(const char *input, uint8_t *output, size_t *output_len);

#endif /* __TAPDISK_UTIL_H__ */
