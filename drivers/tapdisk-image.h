/*
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
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
