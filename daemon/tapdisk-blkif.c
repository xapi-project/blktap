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
#include <stdlib.h>
#include <string.h>

#include "tapdisk-dispatch.h"

struct blkif *
alloc_blkif(domid_t domid)
{
	struct blkif *blkif;

	DPRINTF("Alloc_blkif called [%d]\n", domid);

	blkif = malloc(sizeof(struct blkif));
	if (!blkif)
		return NULL;

	memset(blkif, 0, sizeof(struct blkif));
	blkif->domid = domid;
	blkif->devnum = -1;

	return blkif;
}

void
free_blkif(struct blkif *blkif)
{
	if (!blkif)
		return;

	blkif_unmap(blkif);

	if (blkif->info) {
		free(blkif->info->params);
		free(blkif->info);
	}

	free(blkif->prv);
	free(blkif);
}

int
blkif_init(struct blkif *blkif, long int handle, long int pdev)
{
	int devnum;
	
	if (blkif == NULL)
		return -EINVAL;
	
	blkif->handle = handle;
	blkif->pdev   = pdev;

	if (tapdisk_control_new(blkif)) {
		DPRINTF("BLKIF: Image open failed\n");
		return -1;
	}
	
	devnum = tapdisk_control_map(blkif);
	if (devnum < 0)
		return -1;

	blkif->devnum = devnum;
	
	return 0;
}

void
blkif_unmap(struct blkif *blkif)
{
	if (blkif->minor) {
		blkif->state = DISCONNECTING;
		tapdisk_control_unmap(blkif);
		blkif->major = 0;
		blkif->minor = 0;
		blkif->state = DISCONNECTED;
	}
}

int
blkif_connected(struct blkif *blkif)
{
	int err;

	err = tapdisk_control_connected(blkif);
	if (!err)
		blkif->state = CONNECTED;

	return err;
}

int
blkif_checkpoint(struct blkif *blkif, char *request)
{
	return tapdisk_control_checkpoint(blkif, request);
}

int
blkif_lock(struct blkif *blkif, char *request, int enforce)
{
	return tapdisk_control_lock(blkif, request, enforce);
}
