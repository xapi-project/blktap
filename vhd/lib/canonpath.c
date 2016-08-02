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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "canonpath.h"

/*
 * Return a path name which should be used resolving parent.
 * This could be different from realpath as realpath follows all symlinks.
 * Function try to get a file name (even if symlink) contained in /dev/mapper.
 */
char *
canonpath(const char *path, char *resolved_path)
{
	/* make some cleanup */
	char canon[PATH_MAX], *p, *dst;
	size_t len = strlen(path);

	if (len >= PATH_MAX)
		goto fallback;
	memcpy(canon, path, len+1);

	/* "//" -> "/" */
	while ((p = strstr(canon, "//")) != NULL)
		memmove(p, p+1, strlen(p+1)+1);

	/* "/./" -> "/" */
	while ((p = strstr(canon, "/./")) != NULL)
		memmove(p, p+2, strlen(p+2)+1);

	/*
	 * if path points to a file in /dev/mapper (with no subdirectories)
	 * return it without following symlinks
	 */
	if (strncmp(canon, "/dev/mapper/", 12) == 0 &&
	    strchr(canon+12, '/') == NULL &&
	    access(canon, F_OK) == 0) {
		strcpy(resolved_path, canon);
		return resolved_path;
	}

	/*
	 * if path is in a subdirectory of dev (possibly a logical volume)
	 * try to find a corresponding file in /dev/mapper and return
	 * if found
	 */
	if (strncmp(canon, "/dev/", 5) == 0 &&
	    (p = strchr(canon+5, '/')) != NULL &&
	    strchr(p+1, '/') == NULL) {

		strcpy(resolved_path, "/dev/mapper/");
		dst = strchr(resolved_path, 0);
		for (p = canon+5; *p; ++p) {
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
		if (access(resolved_path, F_OK) == 0)
			return resolved_path;
	}

fallback:
	return realpath(path, resolved_path);
}

