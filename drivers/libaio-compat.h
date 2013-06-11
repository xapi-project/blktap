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
