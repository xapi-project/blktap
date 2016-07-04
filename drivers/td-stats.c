/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
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

#include <stdlib.h>
#include <xenctrl.h>

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
