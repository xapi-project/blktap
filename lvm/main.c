/*
 * Copyright (c) 2021, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

