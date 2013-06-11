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
	int             err;
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
	if (st->err)
		return st->err;

	return st->pos - st->buf;
}

void tapdisk_stats_enter(td_stats_t *st, char t);
void tapdisk_stats_leave(td_stats_t *st, char t);
void tapdisk_stats_field(td_stats_t *st, const char *key, const char *conv, ...);
void tapdisk_stats_val(td_stats_t *st, const char *conv, ...);

#endif /* _TAPDISK_STATS_H_ */
