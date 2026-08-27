/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#ifndef TAPDISK_METRICS_STATS_H
#define TAPDISK_METRICS_STATS_H

#include <stdint.h>

struct stats {
    uint32_t version;
    uint32_t __pad;
    uint64_t oo_reqs;
    uint64_t read_reqs_submitted;
    uint64_t read_reqs_completed;
    uint64_t read_sectors;
    uint64_t read_total_ticks;
    uint64_t write_reqs_submitted;
    uint64_t write_reqs_completed;
    uint64_t write_sectors;
    uint64_t write_total_ticks;
    uint64_t io_errors;
    uint64_t flags;
};

#endif /* TAPDISK_METRICS_STATS_H */
