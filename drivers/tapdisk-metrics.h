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


#ifndef TAPDISK_METRICS_H
#define TAPDISK_METRICS_H

#define TAPDISK_METRICS_PATHF        "/dev/shm/td3-%d"
#define TAPDISK_METRICS_VDI_PATHF    "%s/vdi-%hu"
#define TAPDISK_METRICS_VBD_PATHF    "%s/vbd-%d-%d"
#define TAPDISK_METRICS_BLKTAP_PATHF "%s/blktap-%d"
#define TAPDISK_METRICS_NBD_PATHF "%s/nbd-%d"
#define BT3_LOW_MEMORY_MODE 0x0000000000000001

#include <libaio.h>

#include "tapdisk-utils.h"
#include "tapdisk.h"

struct stats {
    uint32_t version;
    unsigned long long oo_reqs;
    unsigned long long read_reqs_submitted;
    unsigned long long read_reqs_completed;
    unsigned long long read_sectors;
    unsigned long long read_total_ticks;
    unsigned long long write_reqs_submitted;
    unsigned long long write_reqs_completed;
    unsigned long long write_sectors;
    unsigned long long write_total_ticks;
    uint64_t io_errors;
    uint64_t flags;
};

typedef struct {
    struct shm shm;
    struct stats *stats;
} stats_t;

typedef struct {
    char *path;
} td_metrics_t;

/* Creates a folder in which to store tapdisk3 statistics: /dev/shm/td3-<pid> */
int td_metrics_start();
/* Destroys the folder /dev/shm/td3-<pid> and its contents */
void td_metrics_stop();

/* Creates the shm for the file that stores metrics from tapdisk to the vdi */
int td_metrics_vdi_start(int minor, stats_t *vdi_stats);

/* Destroys the files created to store the metrics from tapdisk to the vdi */
int td_metrics_vdi_stop(stats_t *vdi_stats);

/* Creates the metrics file to store the stats from blkfront to tapdisk */
int td_metrics_vbd_start(int domain, int id, stats_t *vbd_stats);

/* Destroys the files created to store metrics from blkfront to tapdisk */
int td_metrics_vbd_stop(stats_t *vbd_stats);

/* Creates the metrics file between tapdisk and blktap */
int td_metrics_blktap_start(int minor, stats_t *blktap_stats);

/* Destroys the metrics file between tapdisk and blktap */
int td_metrics_blktap_stop(stats_t *blktap_stats);

int td_metrics_nbd_start(stats_t *nbd_server, int minor);

int td_metrics_nbd_stop(stats_t *nbd_server);
#endif /* TAPDISK_METRICS_H */
