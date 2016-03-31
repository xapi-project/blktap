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

#ifndef _TIMEOUT_MATH_H_
#define _TIMEOUT_MATH_H_

#define TV_INF                      (struct timeval) {(time_t) - 1, 0}
#define TV_IS_INF(a)                ((a).tv_sec == (time_t) - 1)
#define TV_ZERO                     (struct timeval) {0, 0}
#define TV_BEFORE(a, b)             timercmp(&(a), &(b), <)
#define TV_AFTER(a, b)              (TV_BEFORE((b), (a)))
#define TV_MIN(a, b)                (TV_BEFORE((a), (b)) ? (a) : (b))
#define TV_ADD(a, b, res)           timeradd(&(a), &(b), &(res))
#define TV_SUB(a, b, res)           timersub(&(a), &(b), &(res))
#define TV_SECS(a)                  (struct timeval) {(a), 0}
#define TV_USECS(a)                 (struct timeval) {(a) / 1000000, (a) % 1000000}

#endif
