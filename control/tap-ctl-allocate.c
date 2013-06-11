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
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <libgen.h>
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

	if (!access(devname, F_OK))
		if (unlink(devname)) {
			PERROR("unlink %s", devname);
			return errno;
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

	if (!access(BLKTAP2_CONTROL_DEVICE, R_OK | W_OK))
		return 0;

	memset(name, 0, sizeof(name));

	f = fopen("/proc/misc", "r");
	if (!f) {
		EPRINTF("failed to open /proc/misc: %d\n", errno);
		return errno;
	}

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
	fclose(f);
	return err;
}

static int
tap_ctl_allocate_device(int *minor, char **devname)
{
	char *name;
	int fd, err;
	struct blktap2_handle handle;

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
