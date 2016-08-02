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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libvhd.h"

int
vhd_util_set_field(int argc, char **argv)
{
	long value;
	int err, c;
	vhd_context_t vhd;
	char *name, *field;

	err   = -EINVAL;
	value = 0;
	name  = NULL;
	field = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "n:f:v:h")) != -1) {
		switch (c) {
		case 'n':
			name = optarg;
			break;
		case 'f':
			field = optarg;
			break;
		case 'v':
			err   = 0;
			value = strtol(optarg, NULL, 10);
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!name || !field || optind != argc || err)
		goto usage;

	if (strnlen(field, 25) >= 25) {
		printf("invalid field\n");
		goto usage;
	}

	if (strcmp(field, "hidden") && strcmp(field, "marker")) {
		printf("invalid field %s\n", field);
		goto usage;
	}

	if (value < 0 || value > 255) {
		printf("invalid value %ld\n", value);
		goto usage;
	}

	err = vhd_open(&vhd, name, VHD_OPEN_RDWR);
	if (err) {
		printf("error opening %s: %d\n", name, err);
		return err;
	}

	if (!strcmp(field, "hidden")) {
		vhd.footer.hidden = (char)value;
		err = vhd_write_footer(&vhd, &vhd.footer);
		if (err == -ENOSPC && vhd_type_dynamic(&vhd) && value)
			/* if no space to write the primary footer, at least write the 
			 * backup footer so that it's possible to delete the VDI */
			err = vhd_write_footer_at(&vhd, &vhd.footer, 0);
	} else {
		err = vhd_set_marker(&vhd, (char)value);
	}
		
 	vhd_close(&vhd);
	return err;

usage:
	printf("options: <-n name> <-f field> <-v value> [-h help]\n");
	return -EINVAL;
}
