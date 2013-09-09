/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <stdarg.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "blktap2.h"
#include "tapback.h"

char *
tapback_xs_vread(struct xs_handle * const xs, xs_transaction_t xst,
        const char * const fmt, va_list ap)
{
    char *path, *data = NULL;
    unsigned int len = 0;

    assert(xs);

    if (vasprintf(&path, fmt, ap) == -1)
        goto fail;
    assert(path);

    data = xs_read(xs, xst, path, &len);
    free(path);

    if (!data)
        return NULL;

    /*
     * Make sure the returned string is NULL-terminated.
     */
    if ((len > 0 && data[len - 1] != '\0') || (len == 0 && data[0] != '\0')) {
        char *_data = strndup(data, len);
        if (!_data)
            /* TODO log error */
            goto fail;
        free(data);
        data = _data;
    }

    /*
     * Make sure the returned string does not containing NULL characters, apart
     * from the NULL-terminating one.
     *
     * We should be checking for extraneous NULLs before duplicating the
     * buffer, but this way logic is simplified.
     */
    if ((unsigned int)(strrchr(data, '\0') - data) != len)
        /* TODO log error */
        goto fail;

    return data;
fail:
    free(data);
    return NULL;
}

__printf(3, 4)
char *
tapback_xs_read(struct xs_handle * const xs, xs_transaction_t xst,
        const char * const fmt, ...)
{
    va_list ap;
    char *s;

    assert(xs);

    va_start(ap, fmt);
    s = tapback_xs_vread(xs, xst, fmt, ap);
    va_end(ap);

    return s;
}

char *
tapback_device_read(const vbd_t * const device, const char * const path)
{
    assert(device);
    assert(path);

    return tapback_xs_read(blktap3_daemon.xs, blktap3_daemon.xst,
            "%s/%d/%s/%s", BLKTAP3_BACKEND_PATH, device->domid, device->name,
            path);
}

char *
tapback_device_read_otherend(vbd_t * const device,
        const char * const path)
{
    assert(device);
    assert(path);

    return tapback_xs_read(blktap3_daemon.xs, blktap3_daemon.xst, "%s/%s",
            device->frontend_path, path);
}

__scanf(3, 4)
int
tapback_device_scanf_otherend(vbd_t * const device,
        const char * const path, const char * const fmt, ...)
{
    va_list ap;
    int n = 0;
    char *s = NULL;

    assert(device);
    assert(path);

    if (!(s = tapback_device_read_otherend(device, path)))
        return -1;
    va_start(ap, fmt);
    n = vsscanf(s, fmt, ap);
    free(s);
    va_end(ap);

    return n;
}

__printf(4, 5)
int
tapback_device_printf(vbd_t * const device, const char * const key,
        const bool mkread, const char * const fmt, ...)
{
    va_list ap;
    int err = 0;
    char *path = NULL, *val = NULL;
    bool nerr = false;

    assert(device);
    assert(key);

    if (-1 == asprintf(&path, "%s/%d/%s/%s", BLKTAP3_BACKEND_PATH,
                device->domid, device->name, key)) {
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

    if (!(nerr = xs_write(blktap3_daemon.xs, blktap3_daemon.xst, path, val,
                    strlen(val)))) {
        err = -errno;
        goto fail;
    }

    if (mkread) {
        struct xs_permissions perms = {
            device->domid,
            XS_PERM_READ
        };

        if (!(nerr = xs_set_permissions(blktap3_daemon.xs, blktap3_daemon.xst,
                        path, &perms, 1))) {
            err = -errno;
            goto fail;
        }
    }

fail:
    free(path);
    free(val);

    return err;
}
