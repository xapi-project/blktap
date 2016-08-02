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

#ifndef TAPDISK_METRICS_H
#define TAPDISK_METRICS_H

#define TAPDISK_METRICS_PATHF        "/dev/shm/td3-%d"
#define TAPDISK_METRICS_VDI_PATHF    "%s/vdi-%hu"
#define TAPDISK_METRICS_VBD_PATHF    "%s/vbd-%d-%d"
#define TAPDISK_METRICS_BLKTAP_PATHF "%s/blktap-%d"
#define TAPDISK_METRICS_NBD_PATHF "%s/nbd-%d"

#include <libaio.h>

#include "tapdisk-metrics-stats.h"
#include "tapdisk-utils.h"
#include "tapdisk.h"

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
