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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _GNU_SOURCE
	#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "canonpath.h"
#include "util.h"

char *
normalize_path(const char *path, size_t path_len)
{
	size_t res_len = 0;

	const char *ptr = path;
	const char *end = path + path_len;
	const char *next;

	char *normalized_path;
	if (!path_len || *path != '/') {
		// Relative path.
		char *wd = get_current_dir_name();
		if (!wd)
			return NULL;

		res_len = strlen(wd);
		/* Add 2 to accomodate the / and the \0 */
		if (!(normalized_path = realloc(wd, res_len + path_len + 2))) {
			free(wd);
			return NULL;
		}
	} else if (!(normalized_path = malloc(path_len + 1)))
		return NULL;

	for (ptr = path; ptr < end; ptr = next + 1) {
		size_t len;
		if (!(next = memchr(ptr, '/', end - ptr)))
			next = end;
		len = next - ptr;
		switch (len) {
			case 2:
				if (ptr[0] == '.' && ptr[1] == '.') {
					const char *slash = memrchr(normalized_path, '/', res_len);
					if (slash)
						res_len = slash - normalized_path;
					continue;
				}
				break;
			case 1:
				if (ptr[0] == '.')
					continue;
				break;
			case 0:
				continue;
		}
		normalized_path[res_len++] = '/';
		memcpy(normalized_path + res_len, ptr, len);
		res_len += len;
	}

	if (res_len == 0)
		normalized_path[res_len++] = '/';
	normalized_path[res_len] = '\0';
	return normalized_path;
}

/*
 * Return a path name which should be used resolving parent.
 * This could be different from realpath as realpath follows all symlinks.
 * Function try to get a file name (even if symlink) contained in /dev/mapper.
 * Symlinks are also kept if a /dev/drbd/by-res/<UUID>/<VOLUME_ID> path is used.
 */
char *
canonpath(const char *path, char *resolved_path, size_t dest_size)
{
	static const char dev_path[] = "/dev/";
	static const size_t dev_len = sizeof(dev_path) - 1;

	static const char dev_mapper_path[] = "/dev/mapper/";
	static const size_t dev_mapper_len = sizeof(dev_mapper_path) - 1;

	static const char dev_drbd_res_path[] = "/dev/drbd/by-res/";
	static const size_t dev_drbd_res_len = sizeof(dev_drbd_res_path) - 1;

	static const char dev_drbd_prefix[] = "/dev/drbd";
	static const size_t dev_drbd_prefix_len = sizeof(dev_drbd_prefix) - 1;

	/* make some cleanup */
	char *canon = NULL, *p, *dst;
	size_t len = strlen(path);

	if (len >= PATH_MAX)
		goto fallback;

	if (!(canon = normalize_path(path, len)))
		return NULL;

	/*
	 * If path points to a file in /dev/mapper (with no subdirectories)
	 * return it without following symlinks.
	 */
	if (strncmp(canon, dev_mapper_path, dev_mapper_len) == 0 &&
	    strchr(canon + dev_mapper_len, '/') == NULL &&
	    access(canon, F_OK) == 0) {
		safe_strncpy(resolved_path, canon, dest_size);
		goto end;
	}

	/*
	 * If path is in a subdirectory of dev (possibly a logical volume)
	 * try to find a corresponding file in /dev/mapper and return
	 * if found. The path can also be a DRBD volume, try to find it in
	 * /dev/drbd/by-res/.
	 */
	if (strncmp(canon, dev_path, dev_len) == 0 && (p = strchr(canon + dev_len, '/')) != NULL) {
		if (strchr(p+1, '/') == NULL) {
			safe_strncpy(resolved_path, dev_mapper_path, dest_size);
			dst = strchr(resolved_path, 0);
			for (p = canon + dev_len; *p; ++p) {
				if (dst - resolved_path >= PATH_MAX - 2)
					goto fallback;
				switch (*p) {
				case '/':
					*dst = '-';
					break;
				case '-':
					*dst++ = '-';
					/* fall through */
				default:
					*dst = *p;
				}
				++dst;
			}
			*dst = 0;
		} else if (strncmp(canon + dev_len, dev_drbd_res_path + dev_len, dev_drbd_res_len - dev_len) == 0) {
			/* If the path is a real DRBD path, it's a symlink that points to /dev/drbdXXXX,
			 * where XXXX are digits. */
			if (!realpath(canon, resolved_path)) {
				free(canon);
				return NULL;
			}

			/* Try to match /dev/drbd. */
			if (strncmp(resolved_path, dev_drbd_prefix, dev_drbd_prefix_len) != 0)
				goto end;

			/* Check the digits. */
			errno = 0;
			strtol(resolved_path + dev_drbd_prefix_len, &p, 10);
			if (p == resolved_path + dev_drbd_prefix_len || errno == ERANGE || *p != '\0')
				goto end; /* Cannot parse correctly pattern. */

			safe_strncpy(resolved_path, canon, dest_size);
		} else
			goto fallback;
		if (access(resolved_path, F_OK) == 0)
			goto end;
	}

fallback:
	free(canon);
	return realpath(path, resolved_path);

end:
	free(canon);
	return resolved_path;
}

