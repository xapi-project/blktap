/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __TD_STATS_H__
#define __TD_STATS_H__

#include "blktap3.h"

struct td_xenblkif_stats {
    struct {
        unsigned long long in;
        unsigned long long out;
    } reqs;
    struct {
        unsigned long long in;
        unsigned long long out;
    } kicks;
    struct {
        unsigned long long msg;
        unsigned long long map;
        unsigned long long vbd;
        unsigned long long img;
    } errors;

	struct blkback_stats *xenvbd;
};

#include "td-blkif.h"
struct td_xenblkif;

void
tapdisk_xenblkif_stats(struct td_xenblkif * blkif, td_stats_t * st);

#endif /* __TD_STATS_H__ */
