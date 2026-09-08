/*
 * Copyright (c) 2019, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

extern struct crypto_blkcipher *xts_aes_setup(void);

int xts_aes_setkey(struct crypto_blkcipher *cipher, const uint8_t *key, unsigned int keysize);

typedef uint64_t sector_t;

static inline void
xts_aes_plain_iv_generate(uint8_t *iv, int iv_size, sector_t sector)
{
    memset(iv, 0, iv_size);
    *(uint32_t *)iv = sector & 0xffffffff; /* LITTLE ENDIAN */
}

static inline int
xts_aes_plain_encrypt(struct crypto_blkcipher *xts_tfm, sector_t sector,
		      uint8_t *dst_buf, uint8_t *src_buf, unsigned int nbytes)
{
	uint8_t iv[16];
	int dstlen;
	xts_aes_plain_iv_generate(iv, 16, sector);

	if (!EVP_CipherInit_ex(xts_tfm->en_ctx, NULL, NULL, NULL, iv, -1))
		return -1;
	if (!EVP_CipherUpdate(xts_tfm->en_ctx, dst_buf, &dstlen, src_buf, nbytes))
		return -2;
	/* no need to finalize with XTS when multiple of blocksize */
	return 0;
}

static inline int
xts_aes_plain_decrypt(struct crypto_blkcipher *xts_tfm, sector_t sector,
		      uint8_t *dst_buf, uint8_t *src_buf, unsigned int nbytes)
{
	uint8_t iv[16];
	int dstlen;
	xts_aes_plain_iv_generate(iv, 16, sector);

	if (!EVP_CipherInit_ex(xts_tfm->de_ctx, NULL, NULL, NULL, iv, -1))
		return -1;
	if (!EVP_CipherUpdate(xts_tfm->de_ctx, dst_buf, &dstlen, src_buf, nbytes))
		return -2;
	/* no need to finalize with XTS when multiple of blocksize */
	return 0;
}
