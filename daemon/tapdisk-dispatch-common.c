/*
 * (c) 2005 Andrew Warfield and Julian Chesterfield
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "tapdisk-dispatch.h"

int
strsep_len(const char *str, char c, unsigned int len)
{
	unsigned int i;
	
	for (i = 0; str[i]; i++)
		if (str[i] == c) {
			if (len == 0)
				return i;
			len--;
		}

	return (len == 0) ? i : -ERANGE;
}

void
make_blktap_dev(char *devname, int major, int minor, int perm)
{
	struct stat st;
	
	if (lstat(devname, &st) == 0) {
		DPRINTF("%s device already exists\n", devname);

		/* it already exists, but is it the same major number */
		if (((st.st_rdev>>8) & 0xff) == major)
			return;

		DPRINTF("%s has old major %d\n", devname,
			(unsigned int)((st.st_rdev >> 8) & 0xff));

		if (unlink(devname)) {
			EPRINTF("unlink %s failed: %d\n", devname, errno);
			/* only try again if we succed in deleting it */
			return;
		}
	}

	/* Need to create device */
	if (mkdir(BLKTAP_DEV_DIR, 0755) == 0)
		DPRINTF("Created %s directory\n", BLKTAP_DEV_DIR);

	if (mknod(devname, perm, makedev(major, minor)) == 0)
		DPRINTF("Created %s device\n", devname);
	else
		EPRINTF("mknod %s failed: %d\n", devname, errno);
}
