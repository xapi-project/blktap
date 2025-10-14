#include "xattr.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <features.h>
#include <sys/xattr.h>

#ifndef ENOATTR
# define ENOATTR ENODATA        /* No such attribute */
#endif

int
xattr_get(int fd, const char *name, void *value, size_t size)
{
	if (fgetxattr(fd, name, value, size) == -1) {
		if ((errno == ENOATTR) || (errno == ENOTSUP)) {
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
	if (fsetxattr(fd, name, value, size, 0) == -1)
		return -errno;
	return 0;
}
