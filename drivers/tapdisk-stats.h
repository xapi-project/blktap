/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TAPDISK_STATS_H_
#define _TAPDISK_STATS_H_

#include <string.h>

#define TD_STATS_MAX_DEPTH 8

struct tapdisk_stats_ctx {
	char           *pos;

	char           *buf;
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

static inline ssize_t
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
