/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <stdlib.h>
#include <xenctrl.h>

#include "tapdisk-log.h"
#include "td-stats.h"
#include "td-ctx.h"

#define ASSERT(p)                                      \
    do {                                               \
        if (!(p)) {                                    \
            EPRINTF("%s:%d: FAILED ASSERTION: '%s'\n", \
                     __FILE__, __LINE__, #p);          \
            abort();                                   \
        }                                              \
    } while (0)

void
tapdisk_xenblkif_stats(struct td_xenblkif * blkif, td_stats_t * st)
{
    ASSERT(blkif);
    ASSERT(st);
    ASSERT(blkif->ctx);

    tapdisk_stats_field(st, "pool", "s", blkif->ctx->pool);
    tapdisk_stats_field(st, "domid", "d", blkif->domid);
    tapdisk_stats_field(st, "devid", "d", blkif->devid);

    tapdisk_stats_field(st, "reqs", "[");
    tapdisk_stats_val(st, "llu", blkif->stats.reqs.in);
    tapdisk_stats_val(st, "llu", blkif->stats.reqs.out);
    tapdisk_stats_leave(st, ']');

    tapdisk_stats_field(st, "kicks", "[");
    tapdisk_stats_val(st, "llu", blkif->stats.kicks.in);
    tapdisk_stats_val(st, "llu", blkif->stats.kicks.out);
    tapdisk_stats_leave(st, ']');

    tapdisk_stats_field(st, "errors", "{");
    tapdisk_stats_field(st, "msg", "llu", blkif->stats.errors.msg);
    tapdisk_stats_field(st, "map", "llu", blkif->stats.errors.map);
    tapdisk_stats_field(st, "vbd", "llu", blkif->stats.errors.vbd);
    tapdisk_stats_field(st, "img", "llu", blkif->stats.errors.img);
    tapdisk_stats_leave(st, '}');
}
