/*
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <openssl/evp.h>

#include "libvhd.h"

#define MAX_KEY_SIZE 512

#define ERR(_f, _a...)						\
	do {							\
		syslog(LOG_INFO, "%s: " _f, __func__, ##_a);	\
		fprintf(stderr, "%s: " _f, __func__, ##_a);	\
	} while (0)

static int
vhd_util_read_key(const char *keypath, uint8_t *key,
		  size_t max_size, size_t *out_size)
{
	int fd, err;
	ssize_t size;
	struct stat sb;

	fd = open(keypath, O_RDONLY);
	if (fd == -1) {
		ERR("failed to open %s: %d\n", keypath, errno);
		err = -errno;
		goto out;
	}

	err = fstat(fd, &sb);
	if (err) {
		ERR("failed to stat %s: %d\n", keypath, errno);
		err = -errno;
		goto out;
	}

	size = read(fd, key, max_size);
	if (size == -1) {
		ERR("failed to read key: %d\n", errno);
		err = -errno;
		goto out;
	}

	if (size != sb.st_size) {
		ERR("short read of key\n");
		err = -EIO;
		goto out;
	}

	if (out_size)
		*out_size = size;

out:
	if (fd != -1)
		close(fd);
	return err;
}

/*
 * calculates keyhash by taking a SHA256 hash of @keyhash->nonce + key
 */
int
__vhd_util_calculate_keyhash(struct vhd_keyhash *keyhash,
			     const uint8_t *key, size_t size)
{
	int err;
	EVP_MD_CTX evp;

	err = -1;
	EVP_MD_CTX_init(&evp);
	if (!EVP_DigestInit_ex(&evp, EVP_sha256(), NULL)) {
		ERR("failed to init sha256 context\n");
		goto out;
	}

	if (!EVP_DigestUpdate(&evp, keyhash->nonce, sizeof(keyhash->nonce))) {
		ERR("failed to hash nonce\n");
		goto cleanup;
	}

	if (!EVP_DigestUpdate(&evp, key, size)) {
		ERR("failed to hash key\n");
		goto cleanup;
	}

	if (!EVP_DigestFinal_ex(&evp, keyhash->hash, NULL)) {
		ERR("failed to finalize hash\n");
		goto cleanup;
	}

	err = 0;

cleanup:
	EVP_MD_CTX_cleanup(&evp);
out:
	return err;
}

static int
vhd_util_calculate_keyhash(struct vhd_keyhash *keyhash, const char *keypath)
{
	int err;
	size_t size;
	uint8_t key[MAX_KEY_SIZE];

	size = 0;
	err = vhd_util_read_key(keypath, key, sizeof(key), &size);
	if (err) {
		ERR("failed to read key: %d\n", err);
		goto out;
	}

	err = __vhd_util_calculate_keyhash(keyhash, key, size);
	if (err) {
		ERR("failed to calculate keyhash: %d\n", err);
		goto out;
	}

out:
	memset(key, 0, sizeof(key));
	return err;
}

static int
vhd_util_set_hex(uint8_t *dst, size_t size, const char *hex)
{
	int i, n, err;

	err = 0;

	n = strlen(hex);
	if (n / 2 != size) {
		ERR("invalid size for hex string\n");
		err = -EINVAL;
		goto out;
	}

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)hex[i];
		switch (c) {
		case 0:
			break;
		case '0'...'9':
			c -= '0';
			break;
		case 'a' ... 'f':
			c = c - 'a' + 10;
			break;
		case 'A' ... 'F':
			c = c - 'A' + 10;
			break;
		default:
			ERR("invalid hex digit\n");
			err = -EINVAL;
			goto out;
		}

		if (i & 1)
			dst[i / 2] |= c;
		else
			dst[i / 2] = (c << 4);
	}

out:
	return err;
}

static int
vhd_util_set_keyhash(struct vhd_keyhash *keyhash, const char *keypath,
		     const char *hash, const char *nonce)
{
	int err;

	memset(keyhash, 0, sizeof(*keyhash));

	if (nonce) {
		err = vhd_util_set_hex(keyhash->nonce,
				       sizeof(keyhash->nonce), nonce);
		if (err)
			goto out;
	}

	if (hash) {
		err = vhd_util_set_hex(keyhash->hash,
				       sizeof(keyhash->hash), hash);
		if (err)
			goto out;
	} else {
		err = vhd_util_calculate_keyhash(keyhash, keypath);
		if (err) {
			ERR("failed to calculate keyhash: %d\n", err);
			goto out;
		}
	}

	keyhash->cookie = 1;

out:
	return err;
}

static int
vhd_util_set_key(vhd_context_t *vhd, const char *keypath,
		 const char *hash, const char *nonce)
{
	int err;
	struct vhd_keyhash keyhash;
        uint32_t i, used;

	memset(&keyhash, 0, sizeof(keyhash));

	if (vhd->footer.type == HD_TYPE_FIXED) {
		ERR("can't save key hashes for fixed vhds\n");
		err = -EINVAL;
		goto out;
	}

	if (keypath && hash) {
		ERR("can't provide both keyhash and keypath\n");
		err = -EINVAL;
		goto out;
	}

	err = vhd_get_bat(vhd);
        if (err) {
            ERR("error reading bat: %d\n", err);
            goto out;
        }
        for (i = 0, used = 0; i < vhd->bat.entries; i++)
            if (vhd->bat.bat[i] != DD_BLK_UNUSED)
                used++;
        if (used != 0) {
            ERR("can't save key hashes for non-empty vhds\n");
            err = -EINVAL;
            goto out;
        }


	err = vhd_util_set_keyhash(&keyhash, keypath, hash, nonce);
	if (err)
		goto out;

	err = vhd_set_keyhash(vhd, &keyhash);
	if (err) {
		ERR("failed to set keyhash: %d\n", err);
		goto out;
	}

out:
	return err;
}

static int
vhd_util_check_key(vhd_context_t *vhd, const char *keypath)
{
	int err;
	struct vhd_keyhash vhdhash, keyhash;

	err = vhd_get_keyhash(vhd, &vhdhash);
	if (err) {
		ERR("failed to read keyhash: %d\n", err);
		goto out;
	}

	if (!vhdhash.cookie) {
		ERR("this vhd has no keyhash\n");
		err = -EINVAL;
		goto out;
	}

	memcpy(keyhash.nonce, vhdhash.nonce, sizeof(keyhash.nonce));
	err = vhd_util_calculate_keyhash(&keyhash, keypath);
	if (err) {
		ERR("failed to calculate keyhash: %d\n", err);
		goto out;
	}

	if (memcmp(keyhash.hash, vhdhash.hash, sizeof(keyhash.hash))) {
		ERR("vhd hash doesn't match key hash\n");
		err = -EINVAL;
		goto out;
	}

out:
	return err;
}

int
vhd_util_key(int argc, char **argv)
{
	vhd_context_t vhd;
	const char *name, *nonce, *keypath, *keyhash;
	int err, c, set, check, print, flags, calc;

	err     = -EINVAL;
	set     = 0;
	check   = 0;
	print   = 0;
	calc    = 0;
	name    = NULL;
	nonce   = NULL;
	keypath = NULL;
	keyhash = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:k:N:H:scCph")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'k':
			keypath = optarg;
			break;
		case 'N':
			nonce = optarg;
			break;
		case 'H':
			keyhash = optarg;
			break;
		case 's':
			set = 1;
			break;
		case 'c':
			check = 1;
			break;
		case 'C':
			calc = 1;
			break;
		case 'p':
			print = 1;
			break;
		case 'h':
			err = 0;
		default:
			goto usage;
		}
	}

	if (optind != argc)
		goto usage;

	if (calc) {
		int i;
		struct vhd_keyhash keyhash;
		err = vhd_util_set_keyhash(&keyhash, keypath, NULL, nonce);
		if (err) {
			ERR("calculating keyhash failed: %d\n", err);
			goto out;
		}

		for (i = 0; i < sizeof(keyhash.hash); i++)
			printf("%02x", keyhash.hash[i]);

		printf("\n");
		goto out;
	}

	if (!name)
		goto usage;

	if (set) {
		if (check)
			goto usage;

		if (!(!!keypath ^ !!keyhash))
			goto usage;
	} else if (check) {
		if (!keypath)
			goto usage;

		if (nonce || keyhash)
			goto usage;
	} else if (!print) {
		goto usage;
	}

	flags = (set ? VHD_OPEN_RDWR : VHD_OPEN_RDONLY);
	err = vhd_open(&vhd, name, flags);
	if (err) {
		fprintf(stderr, "failed to open %s: %d\n", name, err);
		goto out;
	}

	if (set) {
		err = vhd_util_set_key(&vhd, keypath, keyhash, nonce);
		if (err)
			fprintf(stderr, "setting key failed: %d\n", err);
	} else if (check) {
		err = vhd_util_check_key(&vhd, keypath);
		if (err)
			fprintf(stderr, "key check failed: %d\n", err);
	}

	if (print) {
		struct vhd_keyhash keyhash;

		err = vhd_get_keyhash(&vhd, &keyhash);
		if (err) {
			fprintf(stderr, "failed to read keyhash: %d\n", err);
		} else {
			if (keyhash.cookie != 1)
				printf("none\n");
			else {
				int i;

				for (i = 0; i < sizeof(keyhash.nonce); i++)
					printf("%02x", keyhash.nonce[i]);

				printf(" ");

				for (i = 0; i < sizeof(keyhash.hash); i++)
					printf("%02x", keyhash.hash[i]);

				printf("\n");
			}
		}
	}

	vhd_close(&vhd);

out:
	return err;

usage:
	fprintf(stderr,
		"usage:\n"
		"-C -k KEYPATH [-N NONCE]: calculate keyhash for KEYPATH\n"
		"-s -n NAME <-k KEYPATH | -H HASH> [-N NONCE]: set keyhash for NAME\n"
		"-c -n NAME <-k KEYPATH>: check keyhash for NAME\n"
		"-p -n NAME: print keyhash for NAME\n"
		"-h help\n");
	return err;
}
