/*
 * Copyright (c) 2011, XenSource Inc.
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

/*
 * Simple log rate limiting. Allow for bursts, then drop messages
 * until some interval expired.
 */

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
	int expired;

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
