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


#ifndef TAPDISK_METRICS_STATS_H
#define TAPDISK_METRICS_STATS_H

#include <stdint.h>

#define BT3_LOW_MEMORY_MODE 0x0000000000000001

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
