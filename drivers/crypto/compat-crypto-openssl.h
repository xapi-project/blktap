/*
 * Copyright (c) 2014 Citrix Systems, Inc.
 */

#ifndef COMPAT_CRYPTO_OPENSSL_H
#define COMPAT_CRYPTO_OPENSSL_H

#include <openssl/evp.h>

struct crypto_blkcipher
{
	EVP_CIPHER_CTX de_ctx;
	EVP_CIPHER_CTX en_ctx;
};

#endif
