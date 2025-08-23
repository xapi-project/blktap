/*
 * Copyright (c) Cloud Software Group, Inc.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include "util.h"

int 
base64_encode_data(const uint8_t *input, size_t input_len, char **output)
{
	BIO *bio, *b64;
	BUF_MEM *bufferPtr;
	
	if (!input || input_len == 0 || !output) {
		return -1;
	}
	
	*output = NULL;
	
	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new(BIO_s_mem());
	if (!b64 || !bio) {
		if (b64) BIO_free(b64);
		if (bio) BIO_free(bio);
		return -1;
	}
	
	bio = BIO_push(b64, bio);
	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	
	if (BIO_write(bio, input, input_len) != (int)input_len) {
		BIO_free_all(bio);
		return -1;
	}
	
	if (BIO_flush(bio) != 1) {
		BIO_free_all(bio);
		return -1;
	}
	
	BIO_get_mem_ptr(bio, &bufferPtr);
	*output = malloc(bufferPtr->length + 1);
	if (!*output) {
		BIO_free_all(bio);
		return -1;
	}
	
	memcpy(*output, bufferPtr->data, bufferPtr->length);
	(*output)[bufferPtr->length] = '\0';
	
	BIO_free_all(bio);
	return 0;
}

int 
base64_decode_key(const char *input, uint8_t *output, size_t *output_len)
{
	BIO *bio, *b64;
	int decoded_len;
	
	if (!input || !output || !output_len || strlen(input) == 0) {
		return -1;
	}
	
	*output_len = 0;
	
	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new_mem_buf((void*)input, strlen(input));
	if (!b64 || !bio) {
		if (b64) BIO_free(b64);
		if (bio) BIO_free(bio);
		return -1;
	}
	
	bio = BIO_push(b64, bio);
	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	
	decoded_len = BIO_read(bio, output, MAX_AES_XTS_PLAIN_KEYSIZE);
	BIO_free_all(bio);
	
	if (decoded_len < 0) {
		return -1;
	}
	
	*output_len = decoded_len;
	return 0;
}
