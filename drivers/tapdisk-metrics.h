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

#define TAPDISK_METRICS_PATHF "/dev/shm/td3-%d"

typedef struct {
    char *path;
} td_metrics_t;

/* Creates a folder in which to store tapdisk3 statistics: /dev/shm/td3-<pid> */
int td_metrics_start();

/* Destroys the folder /dev/shm/td3-<pid> and its contents */
void td_metrics_stop();

#endif /* TAPDISK_METRICS_H */
