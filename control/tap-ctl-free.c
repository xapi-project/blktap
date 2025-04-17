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
#include <sys/file.h>

#include "tap-ctl.h"
#include "blktap.h"

int
tap_ctl_free(const int minor)
{
	char *path = NULL;
	int mfd = -1, fd, err;

	fd = open(BLKTAP2_NP_RUN_DIR, O_RDONLY);
	if (fd == -1) {
		err = -errno;
		EPRINTF("Failed to open runtime directory %d\n", errno);
		return err;
	}

	/* The only way this can fail is with an EINTR or ENOLCK*/
	err = flock(fd, LOCK_EX);
	if (err == -1) {
		err = -errno;
		EPRINTF("Failed to lock runtime directory %d\n", errno);
		goto out;
	}

	err = asprintf(&path, "%s/tapdisk-%d", BLKTAP2_NP_RUN_DIR, minor);
	if (err == -1) {
		err = -errno;
		goto out;
	}
	err = 0;

	/* Non-Blocking lock to check it's not in use */
	mfd = open(path, O_RDONLY);
	if (mfd == -1) {
		err = -errno;
		EPRINTF("Failed to open marker file %s, %d, err=%d\n",
			path, minor, errno);
		goto out;
	}

	err = flock(mfd, LOCK_EX | LOCK_NB);
	if (err == -1) {
		err = -errno;
		EPRINTF("Unable to lock marker file %s, err = %d\n",
			path, errno);
		goto out;
	}

	unlink(path);

out:
	if (path)
		free(path);

	if (mfd != -1) {
		flock(mfd, LOCK_UN);
		close(mfd);
	}

	flock(fd, LOCK_UN);
	close(fd);
	return err;
}
