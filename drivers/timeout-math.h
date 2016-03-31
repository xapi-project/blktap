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

#define TV_INF                      ((time_t) - 1)
#define TV_IS_INF(a)                ((a) == TV_INF)
#define TV_ZERO                     (0)
#define TV_MIN(a, b)                ((a) <= (b) ? (a) : (b))
#define TV_BEFORE(a, b)             ((a).tv_sec < (b))
#define TV_AFTER(a, b)              ((a) > (b))
#define TV_ADD(a, b, res)           (res) = (a).tv_sec + (b)
#define TV_SUB(a, b, res)           (res) = (a) - (b).tv_sec
#define TV_SECS(a)                  (a)

#endif
