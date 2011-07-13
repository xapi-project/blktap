/*
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
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

#ifndef _TAPDISK_BLKTAP_H_
#define _TAPDISK_BLKTAP_H_

typedef struct td_blktap td_blktap_t;
typedef struct td_blktap_req td_blktap_req_t;

#include "blktap.h"
#include "tapdisk-vbd.h"
#include "list.h"

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
};

int tapdisk_blktap_open(const char *, td_vbd_t *, td_blktap_t **);
void tapdisk_blktap_close(td_blktap_t *);

int tapdisk_blktap_create_device(td_blktap_t *, const td_disk_info_t *, int ro);
int tapdisk_blktap_remove_device(td_blktap_t *);

void tapdisk_blktap_stats(td_blktap_t *, td_stats_t *);

#endif /* _TAPDISK_BLKTAP_H_ */
