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

#ifndef _TAPDISK_BLKTAP_H_
#define _TAPDISK_BLKTAP_H_

typedef struct td_blktap td_blktap_t;
typedef struct td_blktap_req td_blktap_req_t;

#include "blktap.h"
#include "tapdisk-vbd.h"
#include "list.h"
#include "tapdisk-metrics.h"

struct td_blktap_stats {
	struct {
		unsigned long long      in;
		unsigned long long      out;
	} reqs;
	struct {
		unsigned long long      in;
		unsigned long long      out;
	} kicks;
};

struct td_blktap {
	int                     minor;
	td_vbd_t               *vbd;

	int                     fd;

	void                   *vma;
	size_t                  vma_size;

	struct blktap_sring    *sring;
	unsigned int            req_cons;
	unsigned int            rsp_prod_pvt;

	int                     event_id;
	void                   *vstart;

	int                     n_reqs;
	td_blktap_req_t        *reqs;
	int                     n_reqs_free;
	td_blktap_req_t       **reqs_free;

	struct list_head        entry;

	struct td_blktap_stats  stats;

        stats_t blktap_stats;
};

int tapdisk_blktap_open(const char *, td_vbd_t *, td_blktap_t **);
void tapdisk_blktap_close(td_blktap_t *);

int tapdisk_blktap_create_device(td_blktap_t *, const td_disk_info_t *, int ro);
int tapdisk_blktap_remove_device(td_blktap_t *);

void tapdisk_blktap_stats(td_blktap_t *, td_stats_t *);

#endif /* _TAPDISK_BLKTAP_H_ */
