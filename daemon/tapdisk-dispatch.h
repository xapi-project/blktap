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
#ifndef TAPDISK_DISPATCH_H
#define TAPDISK_DISPATCH_H

#include <xs.h>
#include "blktaplib.h"

static inline unsigned long long
tapdisk_get_size(blkif_t *blkif)
{
	image_t *img = (image_t *)blkif->prv;
	return img->size;
}

static inline unsigned long
tapdisk_get_secsize(blkif_t *blkif)
{
	image_t *img = (image_t *)blkif->prv;
	return img->secsize;
}

static inline unsigned int
tapdisk_get_info(blkif_t *blkif)
{
	image_t *img = (image_t *)blkif->prv;
	return img->info;
}

static struct blkif_ops tapdisk_ops = {
	.get_size = tapdisk_get_size,
	.get_secsize = tapdisk_get_secsize,
	.get_info = tapdisk_get_info,
};

/* tapdisk-dispatch-common.c */
int strsep_len(const char *str, char c, unsigned int len);
void make_blktap_dev(char *devname, int major, int minor, int perm);

/* tapdisk-control.c */
void tapdisk_control_start(void);
void tapdisk_control_stop(void);
int tapdisk_control_new(struct blkif *blkif);
int tapdisk_control_connected(struct blkif *blkif);
int tapdisk_control_map(struct blkif *blkif);
int tapdisk_control_unmap(struct blkif *blkif);
int tapdisk_control_checkpoint(struct blkif *blkif, char *request);
int tapdisk_control_lock(struct blkif *blkif, char *request, int enforce);
int tapdisk_control_pause(blkif_t *blkif);
int tapdisk_control_resume(blkif_t *blkif);

/* tapdisk-xenstore.c */
int add_control_watch(struct xs_handle *h, const char *path, const char *uuid);
int remove_control_watch(struct xs_handle *h, const char *path);
int tapdisk_control_handle_event(struct xs_handle *h, const char *uuid);

/* tapdisk-blkif.c */
struct blkif *alloc_blkif(domid_t domid);
void free_blkif(struct blkif *blkif);
int blkif_init(struct blkif *blkif, long int handle, long int pdev);
void blkif_unmap(struct blkif *blkif);
int blkif_connected(struct blkif *blkif);
int blkif_checkpoint(struct blkif *blkif, char *request);
int blkif_lock(struct blkif *blkif, char *request, int enforce);

#endif
