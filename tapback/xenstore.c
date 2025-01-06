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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blktap.h"
#include "tapback.h"

char *
tapback_xs_vread(struct xs_handle * const xs, xs_transaction_t xst,
        const char * const fmt, va_list ap)
{
    char *path, *data = NULL;
    unsigned int len = 0;
	int err = 0;

    ASSERT(xs);

    if (vasprintf(&path, fmt, ap) == -1) {
		err = errno;
        WARN(NULL, "failed to vasprintf: %s\n", strerror(err));
        goto fail;
	}
    ASSERT(path);

    data = xs_read(xs, xst, path, &len);
	err = errno;
    free(path);

    if (!data)
        return NULL;

    /*
     * According to the documentation of xs_read the string is NULL-terminated,
     * though in the prototype there was code checking for that, so lets ensure
     * this is correct.
     */
    if (data[len] != '\0') {
        err = EINVAL;
        WARN(NULL, "XenStore value '%.*s' is not NULL-terminated\n", len,
                data);
        goto fail;
    }

    /*
     * Make sure the returned string does not containing NULL characters, apart
     * from the NULL-terminating one.
     */
    if ((unsigned int)(strchr(data, '\0') - data) != len) {
		err = EINVAL;
        /* TODO print extraneous '\0' characters */
        WARN(NULL, "XenStore value '%s' contains extraneous NULLs\n", data);
        goto fail;
	}

    return data;
fail:
    free(data);
	errno = err;
    return NULL;
}

__printf(3, 4)
char *
tapback_xs_read(struct xs_handle * const xs, xs_transaction_t xst,
        const char * const fmt, ...)
{
    va_list ap;
    char *s;

    ASSERT(xs);

    va_start(ap, fmt);
    s = tapback_xs_vread(xs, xst, fmt, ap);
    va_end(ap);

    return s;
}

char *
tapback_device_read(const vbd_t * const device, xs_transaction_t xst,
        const char * const path)
{
    ASSERT(device);
    ASSERT(path);

    return tapback_xs_read(device->backend->xs, xst, "%s/%s/%s",
            device->backend->path, device->name, path);
}

char *
tapback_device_read_otherend(vbd_t * const device, xs_transaction_t xst,
        const char * const path)
{
    ASSERT(device);
    ASSERT(path);
    ASSERT(device->frontend_path);

    return tapback_xs_read(device->backend->xs, xst, "%s/%s",
            device->frontend_path, path);
}

int
tapback_device_scanf_otherend(vbd_t * const device, xs_transaction_t xst,
        const char * const path, const char * const fmt, ...)
{
    va_list ap;
    int n = 0;
    char *s = NULL;

    ASSERT(device);
    ASSERT(path);

    if (!(s = tapback_device_read_otherend(device, xst, path)))
        return -1;
    va_start(ap, fmt);
    n = vsscanf(s, fmt, ap);
    free(s);
    va_end(ap);

    return n;
}

int
tapback_device_printf(vbd_t * const device, xs_transaction_t xst,
        const char * const key, const bool mkread, const char * const fmt, ...)
{
    va_list ap;
    int err = 0;
    char *path = NULL, *val = NULL;
    bool nerr = false;

    ASSERT(device);
    ASSERT(key);

    if (-1 == asprintf(&path, "%s/%s/%s", device->backend->path,
                device->name, key)) {
        err = -errno;
        goto fail;
    }

    va_start(ap, fmt);
    if (-1 == vasprintf(&val, fmt, ap))
        val = NULL;
    va_end(ap);

    if (!val) {
        err = -errno;
        goto fail;
    }

    if (!(nerr = xs_write(device->backend->xs, xst, path, val, strlen(val)))) {
        err = -errno;
        goto fail;
    }

    if (mkread) {
        struct xs_permissions perms[2] = {
			{device->backend->domid, XS_PERM_NONE},
			{device->domid, XS_PERM_READ}
        };

        if (!(nerr = xs_set_permissions(device->backend->xs, xst, path, perms,
                        ARRAY_SIZE(perms)))) {
            err = -errno;
            goto fail;
        }
    }

fail:
    free(path);
    free(val);

    return err;
}

int
tapback_xs_exists(struct xs_handle * const xs, xs_transaction_t xst,
        char *path, const int *len)
{
    int err = 0;
    char *s = NULL;
    bool exists = false;
    char c = '\0';

    ASSERT(xs);
    ASSERT(path);

    if (len) {
        c = path[*len];
        path[*len] = '\0';
    }

    s = tapback_xs_read(xs, xst, "%s", path);
    if (s)
        exists = true;
    else {
        err = errno;
        ASSERT(err != 0);
        if (err == ENOENT) {
            err = 0;
            exists = false;
        }
    }

    free(s);

    if (len)
        path[*len] = c;

    if (!err) {
        if (exists) {
            return 1;
        } else {
            return 0;
        }
    } else {
        return -err;
    }
}
