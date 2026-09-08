/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __TAPDISK_LOGLIMIT_H__
#define __TAPDISK_LOGLIMIT_H__

#include <sys/time.h>
#include "list.h"

typedef struct td_loglimit td_loglimit_t;

struct td_loglimit {
	int burst;
	int interval;

	int count;
	int dropped;

	struct timeval ts;
};

void tapdisk_loglimit_init(td_loglimit_t *rl, int burst, int interval);

int tapdisk_loglimit_pass(td_loglimit_t *);

#endif /* __TAPDISK_LOGLIMIT_H__ */
