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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "tap-ctl.h"

int tap_ctl_info(pid_t pid, unsigned long long *sectors,
		unsigned int *sector_size, unsigned int *info, const int minor)
{
    tapdisk_message_t message;
    int err;
    struct timeval start, now, delta;

    ASSERT(sectors);
    ASSERT(sector_size);
    ASSERT(info);

	gettimeofday(&start, NULL);
	do {
		memset(&message, 0, sizeof(message));
		message.type = TAPDISK_MESSAGE_DISK_INFO;
		message.cookie = minor;

		err = tap_ctl_connect_send_and_receive(pid, &message, NULL);
		if (err)
			return err;

		if (message.type == TAPDISK_MESSAGE_DISK_INFO_RSP) {
			*sectors = message.u.image.sectors;
			*sector_size = message.u.image.sector_size;
			*info = message.u.image.info;
			break;
		} else if (message.type == TAPDISK_MESSAGE_ERROR) {

			err = -message.u.response.error;

			if (err != -EBUSY)
				break;

			sleep(1);

			gettimeofday(&now, NULL);
			timersub(&now, &start, &delta);

		} else {
			err = -EINVAL;
			EPRINTF("got unexpected result '%s' from %d\n",
					tapdisk_message_name(message.type), pid);
			break;
		}
	} while (delta.tv_sec < TAPCTL_COMM_RETRY_TIMEOUT);

	if (delta.tv_sec >= TAPCTL_COMM_RETRY_TIMEOUT)
		err = -ETIMEDOUT;

	if (err)
		EPRINTF("info failed: %s\n", strerror(-err));

	return err;
}
