#include "xattr.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <features.h>

#ifndef ENOATTR
# define ENOATTR ENODATA        /* No such attribute */
#endif

#if defined (__i386__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		228
# define __NR_fgetxattr		231
#elif defined (__sparc__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		171
# define __NR_fgetxattr		177
#elif defined (__ia64__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		1219
# define __NR_fgetxattr		1222
#elif defined (__powerpc__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		211
# define __NR_fgetxattr		214
#elif defined (__x86_64__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		190
# define __NR_fgetxattr		193
#elif defined (__s390__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		226
# define __NR_fgetxattr		229
#elif defined (__arm__)
# define HAVE_XATTR_SYSCALLS 1
# if defined(__ARM_EABI__) || defined(__thumb__)
#  define __NR_SYSCALL_BASE 0
# else
#  define __NR_SYSCALL_BASE 0x900000
# endif
# define __NR_fsetxattr		(__NR_SYSCALL_BASE+228)
# define __NR_fgetxattr		(__NR_SYSCALL_BASE+231)
#elif defined (__mips64)
# define HAVE_XATTR_SYSCALLS 1
# ifdef __LP64__ /* mips64 using n64 ABI */
#  define __NR_Linux 5000
# else /* mips64 using n32 ABI */
#  define __NR_Linux 6000
# endif
# define __NR_fsetxattr		(__NR_Linux + 182)
# define __NR_fgetxattr		(__NR_Linux + 185)
#elif defined (__mips__) /* mips32, or mips64 using o32 ABI */
# define HAVE_XATTR_SYSCALLS 1
# define __NR_Linux 4000
# define __NR_fsetxattr		(__NR_Linux + 226)
# define __NR_fgetxattr		(__NR_Linux + 229)
#elif defined (__alpha__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		384
# define __NR_fgetxattr		387
#elif defined (__mc68000__)
# define HAVE_XATTR_SYSCALLS 1
# define __NR_fsetxattr		225
# define __NR_fgetxattr		228
#else
# warning "Extended attribute syscalls undefined for this architecture"
# define HAVE_XATTR_SYSCALLS 0
#endif

#if HAVE_XATTR_SYSCALLS
# define SYSCALL(args...)	syscall(args)
#else
# define SYSCALL(args...)	( errno = ENOSYS, -1 )
#endif

static ssize_t
_fgetxattr(int fd, const char *name, void *value, size_t size)
{
	return SYSCALL(__NR_fgetxattr, fd, name, value, size);
}

static int
_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
	return SYSCALL(__NR_fsetxattr, fd, name, value, size, flags);
}

int
xattr_get(int fd, const char *name, void *value, size_t size)
{
	if (_fgetxattr(fd, name, value, size) == -1) {
		if (errno == ENOATTR) {
			memset(value, 0, size);
			return 0;
		}
		return -errno;
	}

	return 0;
}

int
xattr_set(int fd, const char *name, const void *value, size_t size)
{
	if (_fsetxattr(fd, name, value, size, 0) == -1)
		return -errno;
	return 0;
}
