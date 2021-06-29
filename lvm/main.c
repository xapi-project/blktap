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
#include <syslog.h>

#include "lvm-util.h"
#include "lvm-util-priv.h"

static int
usage(void)
{
	printf("usage: lvm-util <vgname>\n");
	exit(EINVAL);
}

int
main(int argc, char **argv)
{
	int i, err;
	struct vg vg;
	struct pv *pv;
	struct lv *lv;
	struct lv_segment *seg;

	if (argc != 2)
		usage();

	err = lvm_scan_vg(argv[1], &vg);
	if (err) {
		fprintf(stderr, "scan failed: %d\n", err);
		return (err >= 0 ? err : -err);
	}

	printf("vg %s: extent_size: %"PRIu64", pvs: %d, lvs: %d\n",
	       vg.name, vg.extent_size, vg.pv_cnt, vg.lv_cnt);

	for (i = 0; i < vg.pv_cnt; i++) {
		pv = vg.pvs + i;
		printf("pv %s: start %"PRIu64"\n", pv->name, pv->start);
	}

	for (i = 0; i < vg.lv_cnt; i++) {
		lv  = vg.lvs + i;
		seg = &lv->first_segment;
		printf("lv %s: size: %"PRIu64", segments: %u, type: %u, "
		       "dev: %s, pe_start: %"PRIu64", pe_size: %"PRIu64"\n",
		       lv->name, lv->size, lv->segments, seg->type,
		       seg->device, seg->pe_start, seg->pe_size);
	}

	lvm_free_vg(&vg);
	return 0;
}

