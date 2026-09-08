/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "tap-ctl.h"

int tap_ctl_info(pid_t pid, unsigned long long *sectors,
		unsigned int *sector_size, unsigned int *info, const int minor)
{
    tapdisk_message_t message;
    int err;

    ASSERT(sectors);
    ASSERT(sector_size);
    ASSERT(info);

    memset(&message, 0, sizeof(message));
    message.type = TAPDISK_MESSAGE_DISK_INFO;
	message.cookie = minor;

    err = tap_ctl_connect_send_and_receive(pid, &message, NULL);
    if (err) {
        EPRINTF("failed to get info from tapdisk %d: %s\n", pid,
                strerror(-err));
        return err;
    }

    if (TAPDISK_MESSAGE_DISK_INFO_RSP == message.type) {
        *sectors = message.u.image.sectors;
        *sector_size = message.u.image.sector_size;
        *info = message.u.image.info;
        return 0;
    } else if (TAPDISK_MESSAGE_ERROR == message.type) {
       return -message.u.response.error;
    } else {
        EPRINTF("unexpected reply %d\n", message.type);
        return -EINVAL;
    }
}
