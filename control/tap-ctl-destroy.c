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
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include "tap-ctl.h"
#include "blktap.h"

int
tap_ctl_destroy(const int id, const int minor,
		int force, struct timeval *timeout)
{
	int err;

	err = tap_ctl_close(id, minor, 0, timeout);
	if (err)
		return err;

	err = tap_ctl_detach(id, minor);
	if (err)
		return err;

	err = tap_ctl_free(minor);
	if (err)
		return err;

	return 0;
}
