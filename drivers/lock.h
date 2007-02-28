/*
 *  Copyright (c) 2007 XenSource Inc.
 *  All rights reserved.
 *
 */

#define LEASE_TIME_SECS 5

#if defined(EXCLUSIVE_LOCK)
int lock(char *fn_to_lock, char *uuid, int force);
int unlock(char *fn_to_unlock, char *uuid);
int lock_delta(char *fn_to_check);
#else
int lock(char *fn_to_lock, char *uuid, int force, int readonly);
int unlock(char *fn_to_unlock, char *uuid, int readonly);
int lock_delta(char *fn_to_check);
#endif
