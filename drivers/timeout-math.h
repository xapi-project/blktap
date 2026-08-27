/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TIMEOUT_MATH_H_
#define _TIMEOUT_MATH_H_

#define TV_INF                      (struct timeval) {(time_t) - 1, 0}
#define TV_IS_INF(a)                ((a).tv_sec < 0)
#define TV_ZERO                     (struct timeval) {0, 0}
#define TV_BEFORE(a, b)             timercmp(&(a), &(b), <)
#define TV_AFTER(a, b)              (TV_BEFORE((b), (a)))
#define TV_MIN(a, b)                (TV_BEFORE((a), (b)) ? (a) : (b))
#define TV_ADD(a, b, res)           timeradd(&(a), &(b), &(res))
#define TV_SUB(a, b, res)           timersub(&(a), &(b), &(res))
#define TV_SECS(a)                  (struct timeval) {(a), 0}
#define TV_USECS(a)                 (struct timeval) {(a) / 1000000, (a) % 1000000}

#endif
