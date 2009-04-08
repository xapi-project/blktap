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

/*
 * Incrementally track mean and standard deviation on sample sequences.
 */

struct dispersion {
	int     k;              /* number of samples */
	float   max, min;
	float   mean, S;        /* mean/stdev accumulators */
};

#define  TD_STATS_MEAN(_st) ((_st)->mean)
#define   TD_STATS_VAR(_st) ((_st)->k > 1 ? (_st)->S / ((_st)->k - 1) : 0)
#define TD_STATS_STDEV(_st) sqrt(TD_STATS_VAR(_st))
#define   TD_STATS_MIN(_st) ((_st)->k > 0 ? (_st)->min : 0.0)
#define   TD_STATS_MAX(_st) ((_st)->k > 0 ? (_st)->max : 0.0)

static inline void
td_dispersion_init(struct dispersion *st)
{
	st->k    = 0;
	st->min  = INFINITY;
	st->max  = -INFINITY;
	st->mean = 0.0;
	st->S    = 0.0;
}

static inline void
td_dispersion_add(struct dispersion *st, float val)
{
	float _mean = st->mean;

	++st->k;

	if (val < st->min)
		st->min = val;

	if (val > st->max)
		st->max = val;

	/*
	 * The mean is a simple recurrence:
	 * M(1) := x(1); M(k) := M(k-1) + (x(k) - M(k-1)) / k
	 */
	st->mean += (val - _mean) / st->k;

	/*
	 * Standard deviation is, uuhm, almost as simple (TAOCP Vol.2 4.2.2).
	 * S(1) := 0; S(k) := S(k-1) + (x(k) - M(k-1)) * (x(k) - M(k))
	 */
	st->S += (val - _mean) * (val - st->mean);
}
