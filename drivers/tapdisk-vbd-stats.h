/*
 * Copyright (c) 2009, XenSource Inc.
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

#include <math.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <limits.h>

/*
 * Incrementally track mean and standard deviation on timer samples.
 */

struct dispersion {
	int            k;              /* number of samples */
	struct timeval max, min;
	struct timeval mean;
	double         S;              /* stdev accumulator */
};

#define TV_FLT(_tv)							\
	((double)(_tv).tv_sec + (double)(_tv).tv_usec / 1000000)

#define FLT_TV(_f) ({							\
	struct timeval __tv;						\
	time_t __s = (_f);						\
	__tv.tv_sec = __s;						\
	__tv.tv_usec = ((_f) - __s) * 1000000;				\
	__tv;								\
})

static const struct timeval __tv_zero = { 0, 0 };

#define  TD_STATS_MEAN(_st) ((_st)->mean)

#define __TD_STATS_VAR(_st) ((_st)->k > 1 ? (_st)->S / ((_st)->k - 1) : 0.0)
#define   TD_STATS_VAR(_st) FLT_TV(TD_STATS_VAR(_st))
#define TD_STATS_STDEV(_st) FLT_TV(sqrt(__TD_STATS_VAR(_st)))

#define   TD_STATS_MIN(_st) ((_st)->k > 0 ? (_st)->min : __tv_zero)
#define   TD_STATS_MAX(_st) ((_st)->k > 0 ? (_st)->max : __tv_zero)

static inline void
td_dispersion_init(struct dispersion *st)
{
	memset(st, 0, sizeof(struct dispersion));
	st->min.tv_sec   = LONG_MAX;
	st->max.tv_sec   = LONG_MIN;
}

static inline void
timerdiv(struct timeval *tv, long k, struct timeval *q)
{
	time_t rest;

	q->tv_sec  = tv->tv_sec / k;
	rest       = tv->tv_sec % k;
	q->tv_usec = (tv->tv_usec + rest * 1000000) / k;

	if (q->tv_usec < 0) {
		q->tv_usec += 1000000;
		q->tv_sec  -= 1;
	}
}

static inline void
td_dispersion_add(struct dispersion *st, struct timeval *tv)
{
	struct timeval _mean, delta1, delta2;

	++st->k;

	if (timercmp(tv, &st->min, <))
		st->min = *tv;

	if (timercmp(&st->max, tv, <))
		st->max = *tv;

	/*
	 * The mean is a simple recurrence:
	 * M(1) := x(1); M(k) := M(k-1) + (x(k) - M(k-1)) / k
	 */

	_mean = st->mean;

	timersub(tv, &_mean, &delta1);
	timerdiv(&delta1, st->k, &delta2);

	timeradd(&_mean, &delta2, &st->mean);

	/*
	 * Standard deviation is, uuhm, almost as simple (TAOCP Vol.2 4.2.2).
	 * S(1) := 0; S(k) := S(k-1) + (x(k) - M(k-1)) * (x(k) - M(k))
	 */

	timersub(tv, &st->mean, &delta2);

	st->S += TV_FLT(delta1) * TV_FLT(delta2);
}

static inline void
td_dispersion_add_now(struct dispersion *st)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	td_dispersion_add(st, &now);
}
