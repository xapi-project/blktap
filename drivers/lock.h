/*
 *  Copyright (c) 2007 XenSource Inc.
 *  All rights reserved.
 *
 */

#define DEFAULT_LEASE_TIME_SECS 5

int lock(char *fn_to_lock, char *uuid, int force, int readonly, int *lease_time);
int unlock(char *fn_to_unlock, char *uuid, int readonly);
int lock_delta(char *fn_to_check, int *lease_time);

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
