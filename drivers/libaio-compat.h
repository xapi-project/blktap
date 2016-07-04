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

#ifndef __LIBAIO_COMPAT
#define __LIBAIO_COMPAT

#include <libaio.h>
#include <unistd.h>
#include <sys/syscall.h>

struct __compat_io_iocb_common {
	char             __pad_buf[8];
	char             __pad_nbytes[8];
	long long	offset;
	long long	__pad3;
	unsigned	flags;
	unsigned	resfd;
};

static inline void __io_set_eventfd(struct iocb *iocb, int eventfd)
{
	struct __compat_io_iocb_common *c;
	c = (struct __compat_io_iocb_common*)&iocb->u.c;
	c->flags |= (1 << 0);
	c->resfd = eventfd;
}

#ifndef SYS_eventfd
#ifndef __NR_eventfd
# if defined(__alpha__)
#  define __NR_eventfd		478
# elif defined(__arm__)
#  define __NR_eventfd		(__NR_SYSCALL_BASE+351)
# elif defined(__ia64__)
#  define __NR_eventfd		1309
# elif defined(__i386__)
#  define __NR_eventfd		323
# elif defined(__m68k__)
#  define __NR_eventfd		319
# elif 0 && defined(__mips__)
#  error __NR_eventfd?
#  define __NR_eventfd		(__NR_Linux + 319)
#  define __NR_eventfd		(__NR_Linux + 278)
#  define __NR_eventfd		(__NR_Linux + 282)
# elif defined(__hppa__)
#  define __NR_eventfd		(__NR_Linux + 304)
# elif defined(__PPC__) || defined(__powerpc64__)
#  define __NR_eventfd		307
# elif defined(__s390__) || defined(__s390x__)
#  define __NR_eventfd		318
# elif defined(__sparc__)
#  define __NR_eventfd		313
# elif defined(__x86_64__)
#  define __NR_eventfd		284
# endif
#else
# error __NR_eventfd?
#endif
#define SYS_eventfd __NR_eventfd
#endif

static inline int tapdisk_sys_eventfd(int initval)
{
	return syscall(SYS_eventfd, initval, 0);
}

#endif /* __LIBAIO_COMPAT */
