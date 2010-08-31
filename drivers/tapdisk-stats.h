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

#ifndef _TAPDISK_STATS_H_
#define _TAPDISK_STATS_H_

#include <string.h>

#define TD_STATS_MAX_DEPTH 8

struct tapdisk_stats_ctx {
	void           *pos;

	void           *buf;
	size_t          size;

	int             n_elem[TD_STATS_MAX_DEPTH];
	int             depth;
};

typedef struct tapdisk_stats_ctx td_stats_t;

static inline void
tapdisk_stats_init(td_stats_t *st, char *buf, size_t size)
{
	memset(st, 0, sizeof(*st));

	st->pos  = buf;
	st->buf  = buf;
	st->size = size;
}

static inline size_t
tapdisk_stats_length(td_stats_t *st)
{
	return st->pos - st->buf;
}

void tapdisk_stats_enter(td_stats_t *st, char t);
void tapdisk_stats_leave(td_stats_t *st, char t);
void tapdisk_stats_field(td_stats_t *st, const char *key, const char *conv, ...);
void tapdisk_stats_val(td_stats_t *st, const char *conv, ...);

#endif /* _TAPDISK_STATS_H_ */
