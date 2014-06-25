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
