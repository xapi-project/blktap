/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Simple log rate limiting. Allow for bursts, then drop messages
 * until some interval expired.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "tapdisk-loglimit.h"
#include "compiler.h"
#include "list.h"

void
tapdisk_loglimit_init(td_loglimit_t *rl, int burst, int interval)
{
	rl->burst    = burst;
	rl->interval = interval;

	rl->count    = 0;
	rl->dropped  = 0;

	gettimeofday(&rl->ts, NULL);
}

static void
timeradd_ms(struct timeval *tv, long ms)
{
	tv->tv_usec += ms * 1000;
	if (tv->tv_usec > 1000000) {
		tv->tv_sec  += tv->tv_usec / 1000000;
		tv->tv_usec %= 1000000;
	}
}

static void
tapdisk_loglimit_update(td_loglimit_t *rl, struct timeval *now)
{
	struct timeval next = rl->ts;

	timeradd_ms(&next, rl->interval);

	if (timercmp(&next, now, <)) {
		rl->count = 0;
		rl->ts    = *now;
	}
}

static void
tapdisk_loglimit_update_now(td_loglimit_t *rl)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	tapdisk_loglimit_update(rl, &now);
}

int
tapdisk_loglimit_pass(td_loglimit_t *rl)
{
	if (!rl->interval)
		return 1; /* unlimited */

	if (unlikely(rl->count >= rl->burst)) {

		tapdisk_loglimit_update_now(rl);

		if (rl->count >= rl->burst) {
			rl->dropped++;
			return 0;
		}
	}

	rl->count++;
	return 1;
}
