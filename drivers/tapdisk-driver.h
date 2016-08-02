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

#ifndef _TAPDISK_DRIVER_H_
#define _TAPDISK_DRIVER_H_

#include "tapdisk.h"
#include "scheduler.h"
#include "tapdisk-queue.h"
#include "tapdisk-loglimit.h"

#define TD_DRIVER_OPEN               0x0001
#define TD_DRIVER_RDONLY             0x0002
#define SECTOR_SIZE                  512

struct td_driver_handle {
	int                          type;
	char                        *name;

	int                          storage;

	int                          refcnt;
	td_flag_t                    state;

	td_disk_info_t               info;

	void                        *data;
	const struct tap_disk       *ops;

	td_loglimit_t                loglimit;
	struct list_head             next;
};

td_driver_t *tapdisk_driver_allocate(int, const char *, td_flag_t);
void tapdisk_driver_free(td_driver_t *);

void tapdisk_driver_queue_tiocb(td_driver_t *, struct tiocb *);

void tapdisk_driver_debug(td_driver_t *);

void tapdisk_driver_stats(td_driver_t *, td_stats_t *);

int tapdisk_driver_log_pass(td_driver_t *, const char *caller);

#endif
