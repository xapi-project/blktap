/*
 * Copyright (c) 2010, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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

/*
 * Copyright (c) 2014 Citrix Systems, Inc.
 */


#include <err.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "compat-crypto-openssl.h"
#include "xts_aes.h"

struct crypto_blkcipher * xts_aes_setup(void)
{
	struct crypto_blkcipher *ret;

	ret = calloc(1, sizeof(struct crypto_blkcipher));
	if (!ret)
		return NULL;
	return ret;
}

int xts_aes_setkey(struct crypto_blkcipher *cipher, const uint8_t *key, unsigned int keysize)
{
	const EVP_CIPHER *type;

	switch (keysize) {
	case 64: type = EVP_aes_256_xts(); break;
	case 32: type = EVP_aes_128_xts(); break;
	default: return -21; break;
	}

	if (!type)
		return -20;

	EVP_CIPHER_CTX_init(&cipher->en_ctx);
	EVP_CIPHER_CTX_init(&cipher->de_ctx);

	/* TODO lazily initialize the encrypt context until doing an encryption,
	 * since it's only needed for a writable node (top diff) */
	if (!EVP_CipherInit_ex(&cipher->en_ctx, type, NULL, NULL, NULL, 1))
		return -1;
	if (!EVP_CipherInit_ex(&cipher->de_ctx, type, NULL, NULL, NULL, 0))
		return -2;
	if (!EVP_CIPHER_CTX_set_key_length(&cipher->en_ctx, keysize))
		return -3;
	if (!EVP_CipherInit_ex(&cipher->en_ctx, NULL, NULL, key, NULL, 1))
		return -4;
	if (!EVP_CIPHER_CTX_set_key_length(&cipher->de_ctx, keysize))
		return -5;
	if (!EVP_CipherInit_ex(&cipher->de_ctx, NULL, NULL, key, NULL, 0))
		return -6;
	return 0;
}
