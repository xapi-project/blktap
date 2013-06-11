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
