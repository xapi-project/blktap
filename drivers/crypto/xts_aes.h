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

	if (!EVP_CipherInit_ex(&xts_tfm->en_ctx, NULL, NULL, NULL, iv, -1))
		return -1;
	if (!EVP_CipherUpdate(&xts_tfm->en_ctx, dst_buf, &dstlen, src_buf, nbytes))
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

	if (!EVP_CipherInit_ex(&xts_tfm->de_ctx, NULL, NULL, NULL, iv, -1))
		return -1;
	if (!EVP_CipherUpdate(&xts_tfm->de_ctx, dst_buf, &dstlen, src_buf, nbytes))
		return -2;
	/* no need to finalize with XTS when multiple of blocksize */
	return 0;
}
