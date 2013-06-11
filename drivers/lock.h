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

#define DEFAULT_LEASE_TIME_SECS 30

int lock(char *fn_to_lock, char *uuid, int force, int readonly, int *lease_time, int *retstat);
int unlock(char *fn_to_unlock, char *uuid, int readonly, int *retstat);
int lock_delta(char *fn_to_check, int *cur_lease_time, int *max_lease_time);

typedef enum {
    LOCK_OK          =  0,
    LOCK_EBADPARM    = -1,
    LOCK_ENOMEM      = -2,
    LOCK_ESTAT       = -3,
    LOCK_EHELD_WR    = -4,
    LOCK_EHELD_RD    = -5,
    LOCK_EOPEN       = -6,
    LOCK_EXLOCK_OPEN = -7,
    LOCK_EXLOCK_WRITE= -8,
    LOCK_EINODE      = -9,
    LOCK_EUPDATE     = -10,
    LOCK_EREAD       = -11,
    LOCK_EREMOVE     = -12,
    LOCK_ENOLOCK     = -13,
    LOCK_EUSAGE      = -14,
} lock_error;
