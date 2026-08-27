/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>

#include "debug.h"
#include "tapdisk-log.h"
#include "td-stats.h"
#include "td-ctx.h"

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
