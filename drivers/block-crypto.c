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


#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "list.h"
#include "libvhd.h"
#include "tapdisk.h"
#include "vhd-util.h"
#include "util.h"

#include "crypto/compat-crypto-openssl.h"
#include "crypto/xts_aes.h"

#define MAX_AES_XTS_PLAIN_KEYSIZE 1024

char * vhd_util_get_vhd_basename(vhd_context_t *vhd);
extern int CRYPTO_SUPPORTED_KEYSIZE[];

/*
 * calculates keyhash by taking a SHA256 hash of @keyhash->nonce + key
 */
int
vhd_calculate_keyhash(struct vhd_keyhash *keyhash,
		const uint8_t *key, size_t key_bytes)
{
	int err;
	EVP_MD_CTX *evp = NULL;

	err = -1;
	evp = EVP_MD_CTX_new();
	if (!EVP_DigestInit_ex(evp, EVP_sha256(), NULL)) {
		EPRINTF("failed to init sha256 context\n");
		goto cleanup;
	}

	if (!EVP_DigestUpdate(evp, keyhash->nonce, sizeof(keyhash->nonce))) {
		EPRINTF("failed to hash nonce\n");
		goto cleanup;
	}

	if (!EVP_DigestUpdate(evp, key, key_bytes)) {
		EPRINTF("failed to hash key\n");
		goto cleanup;
	}

	if (!EVP_DigestFinal_ex(evp, keyhash->hash, NULL)) {
		EPRINTF("failed to finalize hash\n");
		goto cleanup;
	}

	err = 0;

cleanup:
	EVP_MD_CTX_free(evp);
	return err;
}

static int
check_key(const uint8_t *keybuf, unsigned int keysize,
	  const struct vhd_keyhash *vhdhash)
{
	int err;
	struct vhd_keyhash keyhash;

	if (!vhdhash->cookie) {
		DPRINTF("missing key hash\n");
		err = 1;
		goto out;
	}

	memcpy(keyhash.nonce, vhdhash->nonce, sizeof(keyhash.nonce));
	err = vhd_calculate_keyhash(&keyhash, keybuf, keysize / 8);
	if (err) {
		DPRINTF("failed to calculate keyhash: %d\n", err);
		goto out;
	}

	if (memcmp(keyhash.hash, vhdhash->hash, sizeof(keyhash.hash))) {
		DPRINTF("key hash mismatch\n");
		err = 1;
		goto out;
	}

out:
	if (err) {
		DPRINTF("key check failed\n");
		err = -ENOKEY;
	}
	return err;
}

#ifdef OPEN_XT
static int
find_keyfile(char **keyfile, const char *dirs,
	     const char *basename, int keysize)
{
	char *sep = NULL;
	*keyfile  = NULL;

	while (dirs && strlen(dirs) > 0) {
		char keydir[256] = { 0 }, path[277] = { 0 };
		struct stat st;
		int err;

		sep = strchr(dirs, ',');
		/* get directory element */
		if (sep == NULL) {
			safe_strncpy(keydir, dirs, sizeof(keydir));
			dirs = NULL;
		} else {
			size_t len = sep - dirs;
			safe_strncpy(keydir, dirs, len);
			dirs = sep+1;
		}

		/* check if keyfile is inside dir */
		snprintf(path, sizeof(path),
			 "%s/%s,aes-xts-plain,%d.key",
			 keydir, basename, keysize);
		err = stat(path, &st);
		if (err == 0) {
			/* found */
			*keyfile = strdup(path);
			if (*keyfile == NULL) {
				return -ENOMEM;
			}
			DPRINTF("found keyfile %s\n", path);
			return 0;
		} else if (err < 0 && errno != ENOENT) {
			return -errno;
		} else {
			DPRINTF("keyfile %s not found\n", path);
		}
	}

	return -ENOENT;
}

static int
read_keyfile(const char *keydir, const char *basename,
	     uint8_t *keybuf, size_t keysize)
{
	int err, fd = -1;
	char *keyfile = NULL;

	err = find_keyfile(&keyfile, keydir, basename, keysize);
	if (err) {
		keyfile = NULL;
		goto out;
	}

	fd = open(keyfile, O_RDONLY);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	err = read(fd, keybuf, keysize / 8);
	if (err != keysize / 8) {
		err = err == -1 ? -errno : -EINVAL;
		goto out;
	}

	DPRINTF("using keyfile %s, keysize %d\n", keyfile, (int)keysize);
	err = 0;

out:
	if (fd != -1)
		close(fd);
	free(keyfile);
	return err;
}

// try 512bit, 256bit keys
static int
read_preferred_keyfile(const char *keydir, const char *basename, uint8_t *keybuf, int *keysize)
{
    int err, i;
    *keysize = 0;
    err = -EINVAL;
    for (i = 0; CRYPTO_SUPPORTED_KEYSIZE[i] > 0; ++i) {
        err = read_keyfile(keydir, basename, keybuf, CRYPTO_SUPPORTED_KEYSIZE[i]);
        if (err == 0) {
            *keysize = CRYPTO_SUPPORTED_KEYSIZE[i];
            return 0;
        }
    }
    return err;
}


static vhd_context_t *
vhd_open_parent(vhd_context_t *ctx)
{
    vhd_context_t *parent = NULL;
    char *next = NULL;
    int err;
    if (ctx->footer.type != HD_TYPE_DIFF)
        goto out;
    if (vhd_parent_raw(ctx))
        goto out;
    err = vhd_parent_locator_get(ctx, &next);
    if (err)
        goto out;

    parent = calloc(1, sizeof(*parent));
    if (!parent)
        goto out;

    err = vhd_open(parent, next, VHD_OPEN_RDONLY);
    if (err) {
        DPRINTF("vhd_open failed: %d\n", err);
        free(parent);
        parent = NULL;
        goto out;
    }
out:
    free(next);
    return parent;
}

/* look up the chain for first parent VHD with encryption key */
static int
chain_find_keyed_vhd(vhd_context_t *vhd, uint8_t *key, int *keysize, struct vhd_keyhash *out_keyhash)
{
    int err;
    struct vhd_keyhash keyhash;
    vhd_context_t *p = vhd, *p2;
    char *basename;
    const char *keydir;
    int found = 0;

    memset(out_keyhash, 0, sizeof(*out_keyhash));

    keydir = getenv("TAPDISK3_CRYPTO_KEYDIR");
    if (keydir == NULL) {
      keydir = CRYPTO_DEFAULT_KEYDIR;
    }

    while (p) {
        err = vhd_get_keyhash(p, &keyhash);
        if (err) {
            DPRINTF("error getting keyhash: %d\n", err);
            return err;
        }

        if (keyhash.cookie && keydir == NULL) {
            DPRINTF("this vhd requires TAPDISK3_CRYPTO_KEYDIR\n");
            return -ENOKEY;
        }

        /* if keydir is set, we check if a key exists (with the same basename)
         * regardless the keyhash.cookie value to prevent an issue where
         * the vhd has been replaced by another one that is clear */
        if (keydir) {
            basename = vhd_util_get_vhd_basename(p);
            if (!basename) {
                err = -ENOMEM;
                goto out;
            }

            err = read_preferred_keyfile(keydir, basename, key, keysize);
            free(basename);
            switch (err) {
            case 0: /* a key has been found with the same basename */
                if (keyhash.cookie == 0) {
                    DPRINTF("key found for %s but no hash set\n", p->file);
                    err = -EACCES;
                    goto out;
                }
                err = check_key(key, *keysize, &keyhash);
                if (err)
                    goto out;
                DPRINTF("using key from vhd: %s\n", p->file);
                *out_keyhash = keyhash;
                found = 1;
                break;
            case -ENOENT: /* no key found, get to the next one if the cookie's not set */
                if (keyhash.cookie != 0) {
                    err = -ENOKEY;
                    goto out;
                }
                break;
            default: /* some another error */
                goto out;
            }
        }

        if (found)
            goto out;

        p2 = p;
        p = vhd_open_parent(p);

        if (p2 != vhd) {
            vhd_close(p2);
            free(p2);
        }
    }
    return 0;
out:
    if (p != vhd) {
        vhd_close(p);
        free(p);
    }
    return err;
}
#endif

int
vhd_open_crypto(vhd_context_t *vhd, const uint8_t *key, size_t key_bytes, const char *name)
{
	struct vhd_keyhash keyhash;
	int err;
#ifdef OPEN_XT
	uint8_t keybuf[MAX_AES_XTS_PLAIN_KEYSIZE / sizeof(uint8_t)] = { 0 };
	key = keybuf;
	int key_bits;
#endif

	if (vhd->xts_tfm)
		return 0;

#ifdef OPEN_XT
	err = chain_find_keyed_vhd(vhd, keybuf, &key_bits, &keyhash);
	if (err) {
	    DPRINTF("error in vhd chain: %d\n", err);
	    return err;
	}

	key_bytes = key_bits / 8;

	if (keyhash.cookie == 0) {
		return 0;
	}
#else
	memset(&keyhash, 0, sizeof(keyhash));
	err = vhd_get_keyhash(vhd, &keyhash);
	if (err) {
		EPRINTF("error getting keyhash: %d\n", err);
		return err;
	}

	if (keyhash.cookie == 0) {
		if (!key) {
			DPRINTF("No crypto, not starting crypto\n");
			return 0;
		}

		EPRINTF("VHD %s has no keyhash when encryption is requested\n", name);
		return -EINVAL;
	} else if (!key) {
		EPRINTF("No encryption key supplied for encrypted VHD, %s\n", name);
		return -EINVAL;
	}

	err = check_key(key, key_bytes * 8, &keyhash);
	if (err) {
		EPRINTF("Keyhash doesn't match vhd key for %s\n", name);
		return err;
	}

	DPRINTF("Keyhash verified, starting crypto for %s\n", name);
#endif

	vhd->xts_tfm = xts_aes_setup();
	if (vhd->xts_tfm == NULL) {
		err = -EINVAL;
		return err;
	}

	xts_aes_setkey(vhd->xts_tfm, key, key_bytes);
	return 0;
}

void
vhd_close_crypto(vhd_context_t *vhd)
{
	if (vhd->xts_tfm)
	{
		EVP_CIPHER_CTX_free(vhd->xts_tfm->en_ctx);
		EVP_CIPHER_CTX_free(vhd->xts_tfm->de_ctx);
		free(vhd->xts_tfm);
	}
}

void
vhd_crypto_decrypt(vhd_context_t *vhd, td_request_t *t)
{
	int sec, ret;

	for (sec = 0; sec < t->secs; sec++) {
		ret = xts_aes_plain_decrypt(vhd->xts_tfm, t->sec + sec,
					    (uint8_t *)t->buf +
					    sec * VHD_SECTOR_SIZE,
					    (uint8_t *)t->buf +
					    sec * VHD_SECTOR_SIZE,
					    VHD_SECTOR_SIZE);
		if (ret) {
			DPRINTF("crypto decrypt failed: %d : TERMINATED\n", ret);
			exit(1); /* XXX */
		}
	}
}

int
vhd_crypto_encrypt_block(vhd_context_t *vhd, sector_t sector, uint8_t *source,
			 uint8_t *dst, unsigned int block_size)
{
	return xts_aes_plain_encrypt(vhd->xts_tfm, sector, dst, source, block_size);
}

void
vhd_crypto_encrypt(vhd_context_t *vhd, td_request_t *t, char *orig_buf)
{
	int sec, ret;

	for (sec = 0; sec < t->secs; sec++) {
		ret = vhd_crypto_encrypt_block(
			vhd, t->sec + sec,
			(uint8_t *)orig_buf + sec * VHD_SECTOR_SIZE,
			(uint8_t *)t->buf + sec * VHD_SECTOR_SIZE,
			VHD_SECTOR_SIZE);
		if (ret) {
			DPRINTF("crypto encrypt failed: %d : TERMINATED\n", ret);
			exit(1); /* XXX */
		}
	}
}

