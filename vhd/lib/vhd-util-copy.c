/*
 * Copyright (c) 2018, Citrix Systems, Inc.
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

/* Would be nice to find a common place for this, also defined in tap-ctl.c */
#define MAX_AES_XTS_PLAIN_KEYSIZE 1024

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>

#include "libvhd.h"

#define LIBBLOCKCRYPTO_NAME "libblockcrypto.so"

typedef int (*vhd_calculate_keyhash)(struct vhd_keyhash *keyhash,
					     const uint8_t *key, size_t key_byte);
typedef int (*vhd_open_crypto)(vhd_context_t *, const uint8_t *, size_t,
				       const char *);
typedef int (*vhd_crypto_encrypt_block)(vhd_context_t *, uint64_t,
					uint8_t *, uint8_t *,
					unsigned int);
vhd_calculate_keyhash pvhd_calculate_keyhash;
vhd_open_crypto pvhd_open_crypto;
vhd_crypto_encrypt_block pvhd_crypto_encrypt_block;
void *crypto_handle;

static int
__load_crypto()
{
	crypto_handle = dlopen(LIBBLOCKCRYPTO_NAME, RTLD_LAZY);
	if (crypto_handle == NULL) {
		return -EINVAL;
	}
	pvhd_calculate_keyhash = (int (*)(struct vhd_keyhash *,
					  const uint8_t *, size_t))
		dlsym(crypto_handle, "vhd_calculate_keyhash");
	if (!pvhd_calculate_keyhash) {
		return -EINVAL;
	}
	pvhd_open_crypto = (int (*)(vhd_context_t *, const uint8_t *, size_t,
				    const char *))
		dlsym (crypto_handle, "vhd_open_crypto");
	if (!pvhd_open_crypto) {
		return -EINVAL;
	}
	pvhd_crypto_encrypt_block = (int (*)(vhd_context_t *, uint64_t,
					     uint8_t *, uint8_t *,
					     unsigned int))
		dlsym (crypto_handle, "vhd_crypto_encrypt_block");
	if (!pvhd_crypto_encrypt_block) {
		return -EINVAL;
	}
	return 0;
}

static int
vhd_encrypt_copy_block(vhd_context_t *source_vhd, vhd_context_t *target_vhd, uint64_t block)
{
	int err;
	int i;
	void *buf;
	char *map;
	uint64_t sec;

	buf = NULL;
	map = NULL;
	sec = block * source_vhd->spb;

	if (source_vhd->bat.bat[block] == DD_BLK_UNUSED)
		return 0;

	err = posix_memalign(&buf, 4096, source_vhd->header.block_size);
	if (err)
		return -err;

	err = vhd_io_read(source_vhd, buf, sec, source_vhd->spb);
	if (err)
		goto done;

	err = vhd_read_bitmap(source_vhd, block, &map);
	if (err)
		goto done;

	if (target_vhd->xts_tfm) {
		/* If the target is encryted, encrypt each block with data */
		for (i = 0; i < source_vhd->spb; i++) {
			if (vhd_bitmap_test(source_vhd, map, i)) {
				void * blk_ptr = buf + i * VHD_SECTOR_SIZE;
				pvhd_crypto_encrypt_block(target_vhd, sec + i, blk_ptr, blk_ptr, VHD_SECTOR_SIZE);
			}
		}
	}

	err = vhd_io_write(target_vhd, buf, sec, source_vhd->spb);
	if (err) {
		printf("Failed to write block %lu : %d\n", block, err);
	}

done:
	free(buf);
	free(map);

	return err;
}

static int
copy_vhd(const char *name, const char *new_name, int key_size, const uint8_t *encryption_key)
{
	int err = 0;
	int i;

	vhd_context_t source_vhd, target_vhd;
	struct vhd_keyhash keyhash;

	source_vhd.fd = -1;
	source_vhd.file = NULL;
	source_vhd.custom_parent = NULL;
	target_vhd.fd = -1;
	target_vhd.file = NULL;
	target_vhd.custom_parent = NULL;

	memset(&keyhash, 0, sizeof(keyhash));

	err = vhd_open(&source_vhd, name, VHD_OPEN_RDONLY);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return -err;
	}

	if (access(new_name, F_OK) != -1) {
		printf("VHD file %s already exists, chose a different name\n",
		       new_name);
		return -EINVAL;
	}

	err = vhd_create(new_name, source_vhd.footer.curr_size,
			 source_vhd.footer.type, source_vhd.footer.curr_size, 0);

	if (err) {
		printf("error creating %s: %d\n", new_name, err);
		goto out;
	}

	err = vhd_open(&target_vhd, new_name, VHD_OPEN_RDWR);
	if (err) {
		printf("error opening %s: %d\n", new_name, err);
		goto out;
	}

	if (encryption_key) {
		err = __load_crypto();
		if(err) {
			printf("failed to load crypto support library %d\n", err);
			goto out;
		}

		err = pvhd_calculate_keyhash(&keyhash, encryption_key, key_size);
		if (err){
			printf("Failed to calculate keyhash %d\n", err);
			goto out;
		}

		keyhash.cookie = 1;

		err = vhd_set_keyhash(&target_vhd, &keyhash);
		if (err) {
			printf("failed to set keyhash %d\n", err);
			goto out;
		}

		err = pvhd_open_crypto(&target_vhd, encryption_key, key_size, new_name);
		if (err) {
			printf("failed to open crypto %d\n", err);
			goto out;
		}
	}

	err = vhd_get_bat(&source_vhd);
	if (err)
		goto out;

	if (vhd_has_batmap(&source_vhd) ) {
		err = vhd_get_batmap(&source_vhd);
		if (err)
			goto out;
	}

	for (i = 0; i < source_vhd.bat.entries; i++) {
		err = vhd_encrypt_copy_block(&source_vhd, &target_vhd, i);
		if (err) {
			printf("Failed to encrypt block %d: %d\n", i, err);
			goto out;
		}
	}

out:
	vhd_close(&source_vhd);
	vhd_close(&target_vhd);

	return err;
}

int read_encryption_key(int fd, uint8_t **encryption_key, int *key_size) 
{
	/* Allocate the space for the key, */
	*encryption_key = malloc(MAX_AES_XTS_PLAIN_KEYSIZE / sizeof(uint8_t));
	if (!*encryption_key) {
		fprintf(stderr, "Failed to allocate space for encrpytion key\n");
		return -1;
	}

	*key_size = read(fd, (void*)*encryption_key,
			MAX_AES_XTS_PLAIN_KEYSIZE / sizeof(uint8_t));
	if (*key_size != 32 && *key_size != 64){
		fprintf(stderr, "Unsupported keysize, use either 256 bit or 512 bit key\n");
		free(*encryption_key);
		return -1;
	}

	return 0;
}

int
vhd_util_copy(int argc, char **argv)
{
	char *name;
	char *new_name;
	char *key_path;
	uint8_t *encryption_key;
	int c;
	int key_fd;
	int key_size;
	int err;

	name = NULL;
	new_name = NULL;
	encryption_key = NULL;
	key_size = 0;


	if (!argc || !argv)
		goto usage;

	optind = 0;

	while ((c = getopt(argc, argv, "n:N:k:Eh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'N':
			new_name = optarg;
			break;
		case 'k':
			if (encryption_key) {
				fprintf(stderr, "Only supply -E or -k once\n");
				exit(1);
			}
			key_path = optarg;
			key_fd = open(key_path, O_RDONLY);
			if (key_fd == -1) {
				printf("Failed to open key file %s: %d\n", key_path, errno);
				return -errno;
			}
			err = read_encryption_key(key_fd, &encryption_key, &key_size);
			close(key_fd);
			if (err) {
				return -err;
			}
			break;
		case 'E':
			if (encryption_key) {
				fprintf(stderr, "Only supply -E or -k once\n");
				exit(1);
			}
			err = read_encryption_key(STDIN_FILENO, &encryption_key, &key_size);
			if (err) {
				return -err;
			}
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	return copy_vhd(name, new_name, key_size, encryption_key);
usage:
	printf("options: <-n name> <-N new VHD name> "
	       "[-k <keyfile> | -E (pass encryption key on stdin)] "
	       "[-h help] \n");
	if (encryption_key) {
		free(encryption_key);
	}
	return -EINVAL;
}
