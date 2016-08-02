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
#include <unistd.h>

#include "libvhd.h"

int
vhd_util_create(int argc, char **argv)
{
	char *name;
	uint64_t size, msize;
	int c, sparse, err;
	vhd_flag_creat_t flags;

	err       = -EINVAL;
	size      = 0;
	msize     = 0;
	sparse    = 1;
	name      = NULL;
	flags     = 0;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:s:S:rh")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 's':
			err  = 0;
			size = strtoull(optarg, NULL, 10);
			break;
		case 'S':
			err = 0;
			msize = strtoull(optarg, NULL, 10);
			break;
		case 'r':
			sparse = 0;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (err || !name || optind != argc)
		goto usage;

	if (msize && msize < size) {
		printf("Error: <-S size> must be greater than <-s size>\n");
		return -EINVAL;
	}

	return vhd_create(name, size << 20,
				  (sparse ? HD_TYPE_DYNAMIC : HD_TYPE_FIXED),
				  msize << 20, flags);

usage:
	printf("options: <-n name> <-s size (MB)> [-r reserve] [-h help] "
			"[<-S size (MB) for metadata preallocation "
			"(see vhd-util resize)>]\n");
	return -EINVAL;
}
