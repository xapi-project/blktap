/*
 * Copyright (c) 2019, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
	int err;

	switch (keysize) {
	case 64: type = EVP_aes_256_xts(); break;
	case 32: type = EVP_aes_128_xts(); break;
	default: return -21; break;
	}

	if (!type)
		return -20;

	cipher->en_ctx = EVP_CIPHER_CTX_new();
	cipher->de_ctx = EVP_CIPHER_CTX_new();

	/* TODO lazily initialize the encrypt context until doing an encryption,
	 * since it's only needed for a writable node (top diff) */
	if (!EVP_CipherInit_ex(cipher->en_ctx, type, NULL, NULL, NULL, 1)) {
		err = -1;
		goto cleanup;
	}
	if (!EVP_CipherInit_ex(cipher->de_ctx, type, NULL, NULL, NULL, 0)) {
		err = -2;
		goto cleanup;
	}
	if (!EVP_CIPHER_CTX_set_key_length(cipher->en_ctx, keysize)) {
		err = -3;
		goto cleanup;
	}
	if (!EVP_CipherInit_ex(cipher->en_ctx, NULL, NULL, key, NULL, 1)) {
		err = -4;
		goto cleanup;
	}
	if (!EVP_CIPHER_CTX_set_key_length(cipher->de_ctx, keysize)) {
		err = -5;
		goto cleanup;
	}
	if (!EVP_CipherInit_ex(cipher->de_ctx, NULL, NULL, key, NULL, 0)) {
		err = -6;
		goto cleanup;
	}

	return 0;

cleanup:
    EVP_CIPHER_CTX_free(cipher->en_ctx);
    EVP_CIPHER_CTX_free(cipher->de_ctx);
    return err;
}
