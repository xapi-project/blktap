/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "tap-ctl.h"
#include "blktap.h"

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
		return -errno;

	start = name;

	for (;;) {
		ptr = strchr(start + 1, '/');
		if (ptr)
			*ptr = '\0';

		err = mkdir(name, 0700);
		if (err && errno != EEXIST) {
			err = -errno;
			PERROR("mkdir %s", name);
			EPRINTF("mkdir failed with %d\n", err);
			break;
		}

		err = 0;

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
tap_ctl_check_environment(void)
{
	int err;

	err = tap_ctl_prepare_directory(BLKTAP2_CONTROL_DIR);
	if (err) {
		EPRINTF("Prepare %s directory failed %d",
				BLKTAP2_CONTROL_DIR, err);
		return err;
	}

	err = tap_ctl_prepare_directory(BLKTAP2_NP_RUN_DIR);
	if (err) {
		EPRINTF("Prepare %s directory failed %d",
				BLKTAP2_NP_RUN_DIR, err);
		return err;
	}

	return err;
}

static int
tap_ctl_allocate_minor(int *minor, char **minor_name)
{
	char *path = NULL;
	struct stat st_buf;
	int err, id, st, f, fid;

	*minor = -1;

	f = open(BLKTAP2_NP_RUN_DIR, O_RDONLY);
	if (f == -1) {
		err = -errno;
		EPRINTF("Failed to open runtime directory %d\n", errno);
		return err;
	}

	/* The only way this can fail is with an EINTR or ENOLCK*/
	err = flock(f, LOCK_EX);
	if (err == -1) {
		err = -errno;
		EPRINTF("Failed to lock runtime directory %d\n", errno);
		goto out;
	}

	for (id=0; id<MAX_ID; id++) {
		err = asprintf(&path, "%s/tapdisk-%d", BLKTAP2_NP_RUN_DIR, id);
		if (err == -1) {
			err = -errno;
			goto out;
		}

		st = stat(path, &st_buf);
		if (st == 0) {
			/* Already exists */
			free(path);
			path = NULL;
			continue;
		}
		if (errno != ENOENT) {
			err = -errno;
			free(path);
			goto out;
		}

		fid = open(path, O_CREAT | O_WRONLY, 0600);
		if (fid == -1) {
			err = -errno;
			EPRINTF("Failed to create ID file %s, %d\n", path, errno);
			free(path);
			goto out;
		}
		close(fid);

		*minor = id;
		*minor_name = path;
		break;
	}

	err = 0;
out:
	flock(f, LOCK_UN);
	close(f);
	return err;
}

int
tap_ctl_allocate(int *minor, char **minor_name)
{
	int err;

	*minor = -1;

	err = tap_ctl_check_environment();
	if (err) {
		EPRINTF("tap-ctl allocate failed check environment");
		return err;
	}

	err = tap_ctl_allocate_minor(minor, minor_name);
	if (err) {
		EPRINTF("tap-ctl allocate failed to allocate device");
		return err;
	}

	return 0;
}
