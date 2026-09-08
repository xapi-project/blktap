/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TAPDISK_DRIVER_H_
#define _TAPDISK_DRIVER_H_

#include "tapdisk.h"
#include "scheduler.h"
#include "io-backend.h"
#include "tapdisk-loglimit.h"
#include "tapdisk-server.h"

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
	q_tiocb                      queue_func;
	p_tiocb                      prep_func;
};

td_driver_t *tapdisk_driver_allocate(int, const char *, td_flag_t);
void tapdisk_driver_free(td_driver_t *);

void tapdisk_driver_queue_tiocb(td_driver_t *, struct tiocb *);

void tapdisk_driver_prep_tiocb(td_driver_t *, struct tiocb *, int, int, char *, size_t,
	long long, td_queue_callback_t, void *);
void tapdisk_driver_debug(td_driver_t *);

void tapdisk_driver_stats(td_driver_t *, td_stats_t *);

int tapdisk_driver_log_pass(td_driver_t *, const char *caller);

#endif
