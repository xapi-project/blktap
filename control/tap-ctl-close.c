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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#include "tap-ctl.h"

int
tap_ctl_close(const int id, const int minor, const int force,
            struct timeval *timeout)
{
    int err;
    tapdisk_message_t message;
    struct timeval start, now, delta;

    /*
     * Keep retrying till tapdisk becomes available
     * to process the close request
     */
     gettimeofday(&start, NULL);
     do {
        memset(&message, 0, sizeof(message));
        message.type = TAPDISK_MESSAGE_CLOSE;
        if (force)
            message.type = TAPDISK_MESSAGE_FORCE_SHUTDOWN;
        message.cookie = minor;

        err = tap_ctl_connect_send_and_receive(id, &message, timeout);
        if (err)
            return err;

        if (message.type == TAPDISK_MESSAGE_CLOSE_RSP
            || message.type == TAPDISK_MESSAGE_ERROR) {
            err = -message.u.response.error;

            if (err != -EBUSY)
                break;

            sleep(1);

            gettimeofday(&now, NULL);
            timersub(&now, &start, &delta);
        }
        else {
            EPRINTF("got unexpected result '%s' from %d\n",
                tapdisk_message_name(message.type), id);
            err = -EINVAL;
            return err;
        }
        /*
         * TODO: Can VBDs be accessed here to get
         * value of TD_VBD_REQUEST_TIMEOUT
         */
    } while(delta.tv_sec < TAPCTL_COMM_RETRY_TIMEOUT);

	if (delta.tv_sec >= TAPCTL_COMM_RETRY_TIMEOUT)
		err = -ETIMEDOUT;

    if (err)
        EPRINTF("close failed: %s\n", strerror(-err));

    return err;
}
