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

#ifndef _BLKTAP_COMPILER_H
#define _BLKTAP_COMPILER_H

#ifdef __GNUC__
#define likely(_cond)           __builtin_expect(!!(_cond), 1)
#define unlikely(_cond)         __builtin_expect(!!(_cond), 0)
#endif

#ifndef likely
#define likely(_cond)           (_cond)
#endif

#ifndef unlikely
#define unlikely(_cond)         (_cond)
#endif

#ifdef __GNUC__
#define __printf(_f, _a)        __attribute__((format (printf, _f, _a)))
#define __scanf(_f, _a)         __attribute__((format (scanf, _f, _a)))
#define __noreturn              __attribute__((noreturn))
#endif

#ifndef __printf
#define __printf(_f, _a)
#endif

#ifndef __scanf
#define __scanf(_f, _a)
#endif

#ifndef __noreturn
#define __noreturn
#endif

#endif /* _BLKTAP_COMPILER_H */
