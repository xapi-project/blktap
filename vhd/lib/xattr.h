#ifndef VHD_XATTR_H
#define VHD_XATTR_H

#include <sys/types.h>

#define VHD_XATTR_MARKER  "user.com.citrix.xenclient.backend.marker"
#define VHD_XATTR_KEYHASH "user.com.citrix.xenclient.backend.keyhash"

int xattr_get(int, const char *, void *, size_t);
int xattr_set(int, const char *, const void *, size_t);

#endif
