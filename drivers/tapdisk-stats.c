/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>

#include "tapdisk.h"
#include "tapdisk-stats.h"

#define BUG_ON(_cond) if (_cond) { td_panic(); }

static void
__stats_vsprintf(td_stats_t *st,
		      const char *fmt, va_list ap)
{
	char *buf;
	int written, new_size, off;
	size_t size = 0;
	written = 1;
	while (written >= size) {
		va_list aq;

		size = st->buf + st->size - st->pos;
		va_copy(aq, ap);
		written = vsnprintf(st->pos, size, fmt, aq);
		va_end(aq);
		if (written < size)
			break;
		new_size = st->size * 2;
		buf = realloc(st->buf, new_size);
		if (!buf) {
			st->err = -ENOMEM;
			written = size;
			break;
		}
		off = st->pos - st->buf;
		st->buf = buf;
		st->size = new_size;
		st->pos = st->buf + off;
	}
	st->pos += written;
}

static void __printf(2, 3)
__stats_sprintf(td_stats_t *st,
		     const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__stats_vsprintf(st, fmt, ap);
	va_end(ap);
}

static void
__stats_enter(td_stats_t *st)
{
	st->depth++;
	BUG_ON(st->depth > TD_STATS_MAX_DEPTH);
	st->n_elem[st->depth] = 0;
}

static void
__stats_leave(td_stats_t *st)
{
	st->depth--;
}

static void
__stats_next(td_stats_t *st)
{
	int n_elem;

	n_elem = st->n_elem[st->depth];
	if (n_elem > 0)
		__stats_sprintf(st, ", ");
	st->n_elem[st->depth]++;
}

static void
__tapdisk_stats_enter(td_stats_t *st, char t)
{
	__stats_sprintf(st, "%c ", t);
	__stats_enter(st);
}

void
tapdisk_stats_enter(td_stats_t *st, char t)
{
	__stats_next(st);
	__tapdisk_stats_enter(st, t);
}

void
tapdisk_stats_leave(td_stats_t *st, char t)
{
	__stats_leave(st);
	__stats_sprintf(st, " %c", t);
}

static void
tapdisk_stats_vval(td_stats_t *st, const char *conv, va_list ap)
{
	char t = conv[0], fmt[32];

	__stats_next(st);

	switch (t) {
	case 's':
		__stats_vsprintf(st, "\"%s\"", ap);
		break;

	default:
		snprintf(fmt, sizeof(fmt), "%%%s", conv);
		__stats_vsprintf(st, fmt, ap);
		break;
	}
}

void
tapdisk_stats_val(td_stats_t *st, const char *conv, ...)
{
	va_list ap;

	va_start(ap, conv);
	tapdisk_stats_vval(st, conv, ap);
	va_end(ap);
}

void
tapdisk_stats_field(td_stats_t *st, const char *key, const char *conv, ...)
{
	va_list ap;
	int n_elem;
	char t;

	n_elem = st->n_elem[st->depth]++;
	if (n_elem > 0)
		__stats_sprintf(st, ", ");

	__stats_sprintf(st, "\"%s\": ", key);

	if (!conv) {
		__stats_sprintf(st, "null");
		return;
	}

	t = conv[0];
	switch (t) {
	case '[':
	case '{':
		__tapdisk_stats_enter(st, t);
		break;
	default:
		va_start(ap, conv);
		__stats_enter(st);
		tapdisk_stats_vval(st, conv, ap);
		__stats_leave(st);
		va_end(ap);
	}
}
