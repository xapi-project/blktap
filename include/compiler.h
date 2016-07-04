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
