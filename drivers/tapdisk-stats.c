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

#include <stdio.h>
#include <stdarg.h>

#include "tapdisk-stats.h"

static void
tapdisk_stats_vsprintf(td_stats_t *st,
		      const char *fmt, va_list ap)
{
	st->pos += vsnprintf(st->pos, st->buf + st->size - st->pos, fmt, ap);
}

static void __attribute__((format (printf, 2, 3)))
tapdisk_stats_sprintf(td_stats_t *st,
		     const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	tapdisk_stats_vsprintf(st, fmt, ap);
	va_end(ap);
}


static inline void
__tapdisk_stats_enter(td_stats_t *st, char t)
{
	tapdisk_stats_sprintf(st, "%c ", t);

	st->depth++;
	st->n_elem[st->depth] = 0;
}

void
tapdisk_stats_enter(td_stats_t *st, char t)
{
	int n_elem;

	n_elem = st->n_elem[st->depth];
	if (n_elem > 0)
		tapdisk_stats_sprintf(st, ", ");
	st->n_elem[st->depth]++;

	__tapdisk_stats_enter(st, t);
}


void
tapdisk_stats_leave(td_stats_t *st, char t)
{
	st->depth--;

	tapdisk_stats_sprintf(st, " %c", t);
}

void
tapdisk_stats_field(td_stats_t *st, const char *key, const char *conv, ...)
{
	va_list ap;
	int n_elem;
	char fmt[32], t;

	n_elem = st->n_elem[st->depth]++;
	if (n_elem > 0)
		tapdisk_stats_sprintf(st, ", ");

	tapdisk_stats_sprintf(st, "\"%s\": ", key);

	if (!conv) {
		tapdisk_stats_sprintf(st, "null");
		return;
	}

	t = conv[0];
	switch (t) {
	case 's':
		va_start(ap, conv);
		tapdisk_stats_vsprintf(st, "\"%s\"", ap);
		va_end(ap);
		break;

	case '[':
	case '{':
		__tapdisk_stats_enter(st, t);
		break;

	default:
		sprintf(fmt, "%%%s", conv);

		va_start(ap, conv);
		tapdisk_stats_vsprintf(st, fmt, ap);
		va_end(ap);
		break;
	}
}
