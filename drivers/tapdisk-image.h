/* 
 * Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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
#ifndef _TAPDISK_IMAGE_H_
#define _TAPDISK_IMAGE_H_

#include "tapdisk.h"

struct td_image_handle {
	int                          type;
	char                        *name;

	td_flag_t                    flags;

	td_driver_t                 *driver;
	td_disk_info_t               info;

	struct list_head             next;

	/*
	 * Basic datapath statistics, in sectors read/written.
	 *
	 * hits:  requests completed by this image.
	 * fail:  requests completed with failure by this image.
	 *
	 * Not that we do not count e.g.
	 * miss:  requests forwarded.
	 * total: requests processed by this image.
	 *
	 * This is because we'd have to compensate for restarts due to
	 * -EBUSY conditions. Those can be extrapolated by following
	 * the chain instead: sum(image[i].hits, i=0..) == vbd.secs;
	 */
	struct {
		td_sector_count_t    hits;
		td_sector_count_t    fail;
	} stats;
};

#define tapdisk_for_each_image(_image, _head)			\
	list_for_each_entry(_image, _head, next)

#define tapdisk_for_each_image_safe(_image, _next, _head)	\
	list_for_each_entry_safe(_image, _next, _head, next)

#define tapdisk_for_each_image_reverse(_image, _head)		\
	list_for_each_entry_reverse(_image, _head, next)

#define tapdisk_image_entry(_head)		\
	list_entry(_head, td_image_t, next)

int tapdisk_image_open(int, const char *, int, td_image_t **);
void tapdisk_image_close(td_image_t *);

int tapdisk_image_open_chain(const char *, int, int, struct list_head *);
void tapdisk_image_close_chain(struct list_head *);
int tapdisk_image_validate_chain(struct list_head *);

td_image_t *tapdisk_image_allocate(const char *, int, td_flag_t);
void tapdisk_image_free(td_image_t *);

int tapdisk_image_check_td_request(td_image_t *, td_request_t);
int tapdisk_image_check_request(td_image_t *, struct td_vbd_request *);
void tapdisk_image_stats(td_image_t *, td_stats_t *);

#endif
