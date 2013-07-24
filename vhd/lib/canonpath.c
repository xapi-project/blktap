/*
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
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

