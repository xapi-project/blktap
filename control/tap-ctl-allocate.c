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
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/major.h>

#include "tap-ctl.h"
#include "blktap2.h"

static int
tap_ctl_prepare_directory(const char *dir)
{
	int err;
	char *ptr, *name, *start;

	err = access(dir, W_OK | R_OK);
	if (!err)
		return 0;

	name = strdup(dir);
	if (!name)
		return ENOMEM;

	start = name;

	for (;;) {
		ptr = strchr(start + 1, '/');
		if (ptr)
			*ptr = '\0';

		err = mkdir(name, 0755);
		if (err && errno != EEXIST) {
			PERROR("mkdir %s", name);
			err = errno;
			break;
		}

		if (!ptr)
			break;
		else {
			*ptr = '/';
			start = ptr + 1;
		}
	}

	free(name);
	return err;
}

static int
tap_ctl_make_device(const char *devname, const int major,
		    const int minor, const int perm)
{
	int err;
	char *copy, *dir;

	copy = strdup(devname);
	if (!copy)
		return ENOMEM;

	dir = dirname(copy);

	err = tap_ctl_prepare_directory(dir);
	free(copy);

	if (err)
		return err;

	if (unlink(devname)) {
		err = errno;
		if (err != ENOENT) {
			PERROR("unlink %s", devname);
			return err;
		}
	}

	err = mknod(devname, perm, makedev(major, minor));
	if (err) {
		PERROR("mknod %s", devname);
		return errno;
	}

	return 0;
}

static int
tap_ctl_check_environment(void)
{
	FILE *f;
	int err, minor;
	char name[256];

	err = tap_ctl_prepare_directory(BLKTAP2_CONTROL_DIR);
	if (err)
		return err;

	f = fopen("/proc/misc", "r");
	if (!f) {
		EPRINTF("failed to open /proc/misc: %d\n", errno);
		return errno;
	}
	/* There is not a lot we can do about an error returned
	 * from flock() so don't check */
	flock(fileno(f), LOCK_EX);

	/* Note err is 0 owing to tap_ctl_prepare_directory() above */
	if (!access(BLKTAP2_CONTROL_DEVICE, R_OK | W_OK))
		goto out;

	memset(name, 0, sizeof(name));

	while (fscanf(f, "%d %256s", &minor, name) == 2)
		if (!strcmp(name, BLKTAP2_CONTROL_NAME)) {
			err = tap_ctl_make_device(BLKTAP2_CONTROL_DEVICE,
						  MISC_MAJOR,
						  minor, S_IFCHR | 0600);
			goto out;
		}

	err = ENOSYS;
	EPRINTF("didn't find %s in /proc/misc\n", BLKTAP2_CONTROL_NAME);

out:
	flock(fileno(f), LOCK_UN);
	fclose(f);
	return err;
}

static int
tap_ctl_allocate_device(int *minor, char **devname)
{
	char *name;
	int fd, err;
	struct blktap2_handle handle;
	char free_devname = 0;

	*minor = -1;
	if (!devname)
		return EINVAL;

	fd = open(BLKTAP2_CONTROL_DEVICE, O_RDONLY);
	if (fd == -1) {
		EPRINTF("failed to open control device: %d\n", errno);
		return errno;
	}

	err = ioctl(fd, BLKTAP2_IOCTL_ALLOC_TAP, &handle);
	close(fd);
	if (err == -1) {
		EPRINTF("failed to allocate new device: %d\n", errno);
		return errno;
	}

	err = asprintf(&name, "%s%d", BLKTAP2_RING_DEVICE, handle.minor);
	if (err == -1) {
		err = ENOMEM;
		goto fail;
	}

	err = tap_ctl_make_device(name, handle.ring,
				  handle.minor, S_IFCHR | 0600);
	free(name);
	if (err) {
		EPRINTF("creating ring device for %d failed: %d\n",
			handle.minor, err);
		goto fail;
	}

	if (*devname)
		name = *devname;
	else {
		err = asprintf(&name, "%s%d",
			       BLKTAP2_IO_DEVICE, handle.minor);
		if (err == -1) {
			err = ENOMEM;
			goto fail;
		}
		*devname = name;
		free_devname = 1;
	}

	err = tap_ctl_make_device(name, handle.device,
				  handle.minor, S_IFBLK | 0600);
	if (err) {
		EPRINTF("creating IO device for %d failed: %d\n",
			handle.minor, err);
		goto fail;
	}

	DBG("new interface: ring: %u, device: %u, minor: %u\n",
	    handle.ring, handle.device, handle.minor);

	*minor = handle.minor;
	return 0;

fail:
	if (free_devname) {
		free(*devname);
		*devname = 0;
	}
	tap_ctl_free(handle.minor);
	return err;
}

int
tap_ctl_allocate(int *minor, char **devname)
{
	int err;

	*minor = -1;

	err = tap_ctl_check_environment();
	if (err)
		return err;

	err = tap_ctl_allocate_device(minor, devname);
	if (err)
		return err;

	return 0;
}
