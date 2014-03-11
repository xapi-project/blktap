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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * required by the Xen headers
 */
#include <inttypes.h>

#include <xen/xen.h>
#include <xen/grant_table.h>
#include <xen/event_channel.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tap-ctl.h"

int
tap_ctl_connect_xenblkif(const pid_t pid, const domid_t domid, const int devid,
	   	const grant_ref_t * grefs, const int order, const evtchn_port_t port,
		int proto, const char *pool, const int minor)
{
    tapdisk_message_t message;
    int i, err;
    struct timeval start, now, delta;

	gettimeofday(&start, NULL);
	do {
		delta = (struct timeval){0, 0};
		memset(&message, 0, sizeof(message));
		message.type = TAPDISK_MESSAGE_XENBLKIF_CONNECT;
		message.cookie = minor;

		message.u.blkif.domid = domid;
		message.u.blkif.devid = devid;
		for (i = 0; i < 1 << order; i++)
			message.u.blkif.gref[i] = grefs[i];
		message.u.blkif.order = order;
		message.u.blkif.port = port;
		message.u.blkif.proto = proto;
		if (pool)
			strncpy(message.u.blkif.pool, pool, sizeof(message.u.blkif.pool));
		else
			message.u.blkif.pool[0] = 0;

		err = tap_ctl_connect_send_and_receive(pid, &message, NULL);
		if (err)
			goto out;

		if (message.type == TAPDISK_MESSAGE_XENBLKIF_CONNECT_RSP
				|| message.type == TAPDISK_MESSAGE_ERROR) {

			err = -message.u.response.error;

			if (err != -EBUSY)
				break;

			sleep(1);

			gettimeofday(&now, NULL);
			timersub(&now, &start, &delta);

		} else {
			EPRINTF("got unexpected result '%s' from tapdisk[%d]\n",
					tapdisk_message_name(message.type), pid);
			err = -EINVAL;
			break;
		}
    } while (delta.tv_sec < TAPCTL_COMM_RETRY_TIMEOUT);

	if (delta.tv_sec >= TAPCTL_COMM_RETRY_TIMEOUT)
		err = -ETIMEDOUT;

out:
	if (err && err != -EALREADY)
		EPRINTF("failed to connect tapdisk[%d] to the ring: %s\n", pid,
				strerror(-err));
    return err;
}

int
tap_ctl_disconnect_xenblkif(const pid_t pid, const domid_t domid,
        const int devid, struct timeval *timeout)
{
    int err;
	tapdisk_message_t message;
    struct timeval start, now, delta;

	gettimeofday(&start, NULL);
	do {
		delta = (struct timeval){0, 0};
		memset(&message, 0, sizeof(message));
		message.type = TAPDISK_MESSAGE_XENBLKIF_DISCONNECT;
		message.u.blkif.domid = domid;
		message.u.blkif.devid = devid;

		err = tap_ctl_connect_send_and_receive(pid, &message, timeout);
		if (err)
			goto out;

		if (message.type == TAPDISK_MESSAGE_XENBLKIF_DISCONNECT_RSP
				|| message.type == TAPDISK_MESSAGE_ERROR) {

			err = -message.u.response.error;

			if (err != -EBUSY)
				break;

			sleep(1);

			gettimeofday(&now, NULL);
			timersub(&now, &start, &delta);

		} else {
			EPRINTF("got unexpected result '%s' from tapdisk[%d]\n",
					tapdisk_message_name(message.type), pid);
			err = -EINVAL;
			break;
		}
    } while (delta.tv_sec < TAPCTL_COMM_RETRY_TIMEOUT);

	if (delta.tv_sec >= TAPCTL_COMM_RETRY_TIMEOUT)
		err = -ETIMEDOUT;

out:
	if (err)
		EPRINTF("failed to disconnect tapdisk[%d] from the ring: %s\n", pid,
				strerror(-err));

    return err;
}
