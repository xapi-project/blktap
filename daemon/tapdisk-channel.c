/*
 * Copyright (c) 2008 Xensource Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/resource.h>

#include <xs.h>
#include "disktypes.h"
#include "tapdisk-dispatch.h"

static inline const char*
tapdisk_channel_state_name(channel_state_t state)
{
	switch (state) {
	case TAPDISK_CHANNEL_DEAD:
		return "dead";
	case TAPDISK_CHANNEL_LAUNCHED:
		return "launched";
	case TAPDISK_CHANNEL_WAIT_PID:
		return "wait-pid";
	case TAPDISK_CHANNEL_PID:
		return "pid";
	case TAPDISK_CHANNEL_WAIT_OPEN:
		return "wait-open";
	case TAPDISK_CHANNEL_RUNNING:
		return "running";
	case TAPDISK_CHANNEL_WAIT_PAUSE:
		return "wait-pause";
	case TAPDISK_CHANNEL_PAUSED:
		return "paused";
	case TAPDISK_CHANNEL_WAIT_RESUME:
		return "wait-resume";
	case TAPDISK_CHANNEL_WAIT_CLOSE:
		return "wait-close";
	case TAPDISK_CHANNEL_CLOSED:
		return "closed";
	}

	return "unknown";
}

static inline const char*
tapdisk_channel_vbd_state_name(vbd_state_t state)
{
	switch (state) {
	case TAPDISK_VBD_UNPAUSED:
		return "unpaused";
	case TAPDISK_VBD_PAUSING:
		return "pausing";
	case TAPDISK_VBD_PAUSED:
		return "paused";
	case TAPDISK_VBD_BROKEN:
		return "broken";
	case TAPDISK_VBD_DEAD:
		return "dead";
	case TAPDISK_VBD_RECYCLED:
		return "recycled";
	}

	return "unknown";
}

static inline int
tapdisk_channel_enter_vbd_state(tapdisk_channel_t *channel, vbd_state_t state)
{
	int err = 0;

	if (channel->vbd_state == TAPDISK_VBD_BROKEN ||
	    channel->vbd_state == TAPDISK_VBD_DEAD)
		err = -EINVAL;

	DPRINTF("%s: vbd state %s -> %s: %d",
		channel->path,
		tapdisk_channel_vbd_state_name(channel->vbd_state),
		tapdisk_channel_vbd_state_name(state),
		err);

	if (!err)
		channel->vbd_state = state;

	return err;
}

static inline const char*
tapdisk_channel_shutdown_state_name(shutdown_state_t state)
{
	switch (state) {
	case TAPDISK_VBD_UP:
		return "up";
	case TAPDISK_VBD_DOWN:
		return "down";
	}

	return "unknown";
}

static int tapdisk_channel_write_atomic(tapdisk_channel_t *,
					const char *, const void *,
					unsigned int);
static void tapdisk_channel_error(tapdisk_channel_t *,
				  const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
static void tapdisk_channel_fatal(tapdisk_channel_t *,
				  const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
static int tapdisk_channel_refresh_params(tapdisk_channel_t *);
static int tapdisk_channel_connect(tapdisk_channel_t *);
static void tapdisk_channel_close_tapdisk(tapdisk_channel_t *);
static void tapdisk_channel_destroy(tapdisk_channel_t *);

int
tapdisk_channel_check_uuid(tapdisk_channel_t *channel)
{
	uint32_t uuid;
	char *uuid_str;

	uuid_str = xs_read(channel->xsh, XBT_NULL, channel->uuid_str, NULL);
	if (!uuid_str)
		return -errno;

	uuid = strtoul(uuid_str, NULL, 10);
	free(uuid_str);

	if (uuid != channel->cookie)
		return -EINVAL;

	return 0;
}

static inline int
tapdisk_channel_validate_watch(tapdisk_channel_t *channel, const char *path)
{
	int err, len;

	len = strsep_len(path, '/', 7);
	if (len < 0)
		return -EINVAL;

	err = tapdisk_channel_check_uuid(channel);
	if (err)
		return -ENOENT;

	return 0;
}

static inline int
tapdisk_channel_validate_message(tapdisk_channel_t *channel,
				 tapdisk_message_t *message)
{
	switch (message->type) {
	case TAPDISK_MESSAGE_PID_RSP:
		if (channel->state != TAPDISK_CHANNEL_WAIT_PID)
			return -EINVAL;
		break;

	case TAPDISK_MESSAGE_OPEN_RSP:
		if (channel->state != TAPDISK_CHANNEL_WAIT_OPEN)
			return -EINVAL;
		break;

	case TAPDISK_MESSAGE_PAUSE_RSP:
		if (channel->state != TAPDISK_CHANNEL_WAIT_PAUSE)
			return -EINVAL;
		break;

	case TAPDISK_MESSAGE_RESUME_RSP:
		if (channel->state != TAPDISK_CHANNEL_WAIT_RESUME)
			return -EINVAL;
		break;

	case TAPDISK_MESSAGE_CLOSE_RSP:
		if (channel->state != TAPDISK_CHANNEL_WAIT_CLOSE)
			return -EINVAL;
		break;

	case TAPDISK_MESSAGE_RUNTIME_ERROR:
		/*
		 * runtime errors can be received at any time
		 * and should not affect the state machine
		 */
		return 0;
	}

	return 0;
}

static int
tapdisk_channel_send_message(tapdisk_channel_t *channel,
			     tapdisk_message_t *message, int timeout)
{
	fd_set writefds;
	struct timeval tv;
	int ret, len, offset;

	tv.tv_sec  = timeout;
	tv.tv_usec = 0;
	offset     = 0;
	len        = sizeof(tapdisk_message_t);

	DPRINTF("%s: sending '%s' message to %d:%d, state %s\n",
		channel->path, tapdisk_message_name(message->type),
		channel->channel_id, channel->cookie,
		tapdisk_channel_state_name(channel->state));

	if (!TAPDISK_CHANNEL_IPC_IDLE(channel))
		EPRINTF("%s: writing message to non-idle channel, state %s (%d)\n",
			channel->path,
			tapdisk_channel_state_name(channel->state),
			channel->state);

	while (offset < len) {
		FD_ZERO(&writefds);
		FD_SET(channel->write_fd, &writefds);

		/* we don't bother reinitializing tv. at worst, it will wait a
		 * bit more time than expected. */

		ret = select(channel->write_fd + 1,
			     NULL, &writefds, NULL, &tv);
		if (ret == -1)
			break;
		else if (FD_ISSET(channel->write_fd, &writefds)) {
			ret = write(channel->write_fd,
				    message + offset, len - offset);
			if (ret <= 0)
				break;
			offset += ret;
		} else
			break;
	}

	if (offset != len) {
		EPRINTF("%s: error writing '%s' message to %d:%d\n",
			channel->path, tapdisk_message_name(message->type),
			channel->channel_id, channel->cookie);
		return -EIO;
	}

	switch (message->type) {
	case TAPDISK_MESSAGE_PID:
		channel->state = TAPDISK_CHANNEL_WAIT_PID;
		break;

	case TAPDISK_MESSAGE_OPEN:
		channel->state = TAPDISK_CHANNEL_WAIT_OPEN;
		break;

	case TAPDISK_MESSAGE_PAUSE:
		channel->state = TAPDISK_CHANNEL_WAIT_PAUSE;
		break;

	case TAPDISK_MESSAGE_RESUME:
		channel->state = TAPDISK_CHANNEL_WAIT_RESUME;
		break;

	case TAPDISK_MESSAGE_CLOSE:
		channel->state = TAPDISK_CHANNEL_WAIT_CLOSE;
		break;

	case TAPDISK_MESSAGE_FORCE_SHUTDOWN:
		channel->state = TAPDISK_CHANNEL_WAIT_CLOSE;
		break;

	default:
		EPRINTF("%s: unrecognized message type %d\n",
			channel->path, message->type);
	}

	return 0;
}

static void
__tapdisk_channel_error(tapdisk_channel_t *channel,
			const char *fmt, va_list ap)
{
	int err;
	char *dir, *buf, *message;

	err = vasprintf(&buf, fmt, ap);
	if (err == -1) {
		EPRINTF("failed to allocate error message\n");
		buf = NULL;
	}

	if (buf)
		message = buf;
	else
		message = "tapdisk error";

	EPRINTF("%s: %s\n", channel->path, message);

	err = asprintf(&dir, "%s/tapdisk-error", channel->path);
	if (err == -1) {
		EPRINTF("%s: failed to write %s\n", __func__, message);
		dir = NULL;
		goto out;
	}

	tapdisk_channel_write_atomic(channel, dir, message, strlen(message));

out:
	free(dir);
	free(buf);
}

static void
tapdisk_channel_error(tapdisk_channel_t *channel, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__tapdisk_channel_error(channel, fmt, ap);
	va_end(ap);
}

static void
tapdisk_channel_fatal(tapdisk_channel_t *channel, const char *fmt, ...)
{
	va_list ap;

	tapdisk_channel_enter_vbd_state(channel, TAPDISK_VBD_BROKEN);

	va_start(ap, fmt);
	__tapdisk_channel_error(channel, fmt, ap);
	va_end(ap);
}

static int
tapdisk_channel_connect_backdev(tapdisk_channel_t *channel)
{
	int err, major, minor;
	char *s, *path, *devname;

	s       = NULL;
	path    = NULL;
	devname = NULL;

	err = ioctl(channel->blktap_fd,
		    BLKTAP_IOCTL_BACKDEV_SETUP, channel->minor);
	if (err) {
		err = -errno;
		goto fail;
	}

	err = asprintf(&path, "%s/backdev-node", channel->path);
	if (err == -1) {
		path = NULL;
		err  = -ENOMEM;
		goto fail;
	}

	s = xs_read(channel->xsh, XBT_NULL, path, NULL);
	if (!s) {
		err = -errno;
		goto fail;
	}

	err = sscanf(s, "%d:%d", &major, &minor);
	if (err != 2) {
		err = -EINVAL;
		goto fail;
	}

	err = asprintf(&devname,"%s/%s%d",
		       BLKTAP_DEV_DIR, BACKDEV_NAME, minor);
	if (err == -1) {
		devname = NULL;
		err = -ENOMEM;
		goto fail;
	}

	err = make_blktap_device(devname, major, minor, S_IFBLK | 0600);
	if (err)
		goto fail;

	free(path);
	err = asprintf(&path, "%s/backdev-path", channel->path);
	if (err == -1) {
		path = NULL;
		err  = -ENOMEM;
		goto fail;
	}

	err = xs_write(channel->xsh, XBT_NULL, path, devname, strlen(devname));
	if (err == 0) {
		err = -errno;
		goto fail;
	}

	err = 0;
 out:
	free(devname);
	free(path);
	free(s);
	return err;

 fail:
	EPRINTF("backdev setup failed [%d]\n", err);
	goto out;
}

static int
tapdisk_channel_complete_connection(tapdisk_channel_t *channel)
{
	int err;
	char *path;

	if (!xs_printf(channel->xsh, channel->path,
		       "tapdisk-pid", "%d", channel->tapdisk_pid)) {
		EPRINTF("ERROR: Failed writing tapdisk-pid");
		return -errno;
	}

	if (!xs_printf(channel->xsh, channel->path,
		       "sectors", "%llu", channel->image.size)) {
		EPRINTF("ERROR: Failed writing sectors");
		return -errno;
	}

	if (!xs_printf(channel->xsh, channel->path,
		       "sector-size", "%lu", channel->image.secsize)) {
		EPRINTF("ERROR: Failed writing sector-size");
		return -errno;
	}

	if (!xs_printf(channel->xsh, channel->path,
		       "info", "%u", channel->image.info)) {
		EPRINTF("ERROR: Failed writing info");
		return -errno;
	}

	err = tapdisk_channel_connect_backdev(channel);
	if (err)
		goto clean;

	return 0;

 clean:
	if (asprintf(&path, "%s/info", channel->path) == -1)
		return err;

	if (!xs_rm(channel->xsh, XBT_NULL, path))
		goto clean_out;

	free(path);
	if (asprintf(&path, "%s/sector-size", channel->path) == -1)
		return err;

	if (!xs_rm(channel->xsh, XBT_NULL, path))
		goto clean_out;

	free(path);
	if (asprintf(&path, "%s/sectors", channel->path) == -1)
		return err;

	if (!xs_rm(channel->xsh, XBT_NULL, path))
		goto clean_out;

	free(path);
	if (asprintf(&path, "%s/tapdisk-pid", channel->path) == -1)
		return err;

	xs_rm(channel->xsh, XBT_NULL, path);

 clean_out:
	free(path);
	return err;
}

static int
tapdisk_channel_send_open_request(tapdisk_channel_t *channel)
{
	int len;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(tapdisk_message_t));

	len = strlen(channel->vdi_path);

	message.type              = TAPDISK_MESSAGE_OPEN;
	message.cookie            = channel->cookie;
	message.drivertype        = channel->drivertype;
	message.u.params.storage  = channel->storage;
	message.u.params.devnum   = channel->minor;
	message.u.params.domid    = channel->domid;
	message.u.params.path_len = len;
	strncpy(message.u.params.path, channel->vdi_path, len);

	if (channel->mode == 'r')
		message.u.params.flags |= TAPDISK_MESSAGE_FLAG_RDONLY;
	if (channel->shared)
		message.u.params.flags |= TAPDISK_MESSAGE_FLAG_SHARED;

	/* TODO: clean this up */
	if (xs_exists(channel->xsh, "/local/domain/0/tapdisk/add-cache"))
		message.u.params.flags |= TAPDISK_MESSAGE_FLAG_ADD_CACHE;
	if (xs_exists(channel->xsh, "/local/domain/0/tapdisk/vhd-index"))
		message.u.params.flags |= TAPDISK_MESSAGE_FLAG_VHD_INDEX;
	if (xs_exists(channel->xsh, "/local/domain/0/tapdisk/log-dirty"))
		message.u.params.flags |= TAPDISK_MESSAGE_FLAG_LOG_DIRTY;

	return tapdisk_channel_send_message(channel, &message, 2);
}

static int
tapdisk_channel_receive_open_response(tapdisk_channel_t *channel,
				      tapdisk_message_t *message)
{
	int err;

	channel->state = TAPDISK_CHANNEL_RUNNING;

	channel->image.size    = message->u.image.sectors;
	channel->image.secsize = message->u.image.sector_size;
	channel->image.info    = message->u.image.info;

	err = tapdisk_channel_complete_connection(channel);
	if (err) {
		tapdisk_channel_fatal(channel,
				      "failure completing connection: %d", err);
		return err;
	}

	return 0;
}

static int
tapdisk_channel_send_shutdown_request(tapdisk_channel_t *channel)
{
	tapdisk_message_t message;

	memset(&message, 0, sizeof(tapdisk_message_t));

	message.type       = TAPDISK_MESSAGE_CLOSE;
	message.drivertype = channel->drivertype;
	message.cookie     = channel->cookie;

	return tapdisk_channel_send_message(channel, &message, 2);
}

static int
tapdisk_channel_send_force_shutdown_request(tapdisk_channel_t *channel)
{
	tapdisk_message_t message;

	memset(&message, 0, sizeof(tapdisk_message_t));

	message.type       = TAPDISK_MESSAGE_FORCE_SHUTDOWN;
	message.drivertype = channel->drivertype;
	message.cookie     = channel->cookie;
	
	return tapdisk_channel_send_message(channel, &message, 2);
}


static int
tapdisk_channel_receive_shutdown_response(tapdisk_channel_t *channel,
					  tapdisk_message_t *message)
{
	channel->state = TAPDISK_CHANNEL_CLOSED;
	tapdisk_channel_close_tapdisk(channel);
	return 0;
}

static int
tapdisk_channel_receive_runtime_error(tapdisk_channel_t *channel,
				      tapdisk_message_t *message)
{
	tapdisk_channel_error(channel,
			      "runtime error: %s", message->u.string.text);
	return 0;
}

static int
tapdisk_channel_send_pid_request(tapdisk_channel_t *channel)
{
	int err;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(tapdisk_message_t));

	message.type       = TAPDISK_MESSAGE_PID;
	message.drivertype = channel->drivertype;
	message.cookie     = channel->cookie;

	err = tapdisk_channel_send_message(channel, &message, 2);

	return err;
}

static int
tapdisk_channel_receive_pid_response(tapdisk_channel_t *channel,
				     tapdisk_message_t *message)
{
	int err;

	channel->state       = TAPDISK_CHANNEL_PID;
	channel->tapdisk_pid = message->u.tapdisk_pid;
	DPRINTF("%s: tapdisk pid: %d\n", channel->path, channel->tapdisk_pid);

	err = setpriority(PRIO_PROCESS, channel->tapdisk_pid, PRIO_SPECIAL_IO);
	if (err) {
		tapdisk_channel_fatal(channel,
				      "setting tapdisk priority: %d", err);
		return err;
	}

	return 0;
}

static int
tapdisk_channel_send_pause_request(tapdisk_channel_t *channel)
{
	tapdisk_message_t message;

	memset(&message, 0, sizeof(tapdisk_message_t));

	DPRINTF("pausing %s\n", channel->path);

	message.type       = TAPDISK_MESSAGE_PAUSE;
	message.drivertype = channel->drivertype;
	message.cookie     = channel->cookie;

	return tapdisk_channel_send_message(channel, &message, 2);
}

static int
tapdisk_channel_write_atomic(tapdisk_channel_t *channel,
			     const char *_path, const void *_data,
			     unsigned int _len)
{
	xs_transaction_t xbt;
	int err, abort;
	unsigned int len;
	void *data;
	bool ok;

again:
	err = 0;

	xbt = xs_transaction_start(channel->xsh);
	if (!xbt) {
		err = -errno;
		EPRINTF("error starting transaction: %d\n", err);
		return err;
	}

	abort = 1;

	data = xs_read(channel->xsh, xbt, channel->path, &len);
	if (!data) {
		err = -errno;
		if (err == -ENOENT)
			goto abort;
		EPRINTF("error reading %s: %d\n", channel->path, err);
		goto abort;
	}

	ok = xs_write(channel->xsh, xbt, _path, _data, _len);
	if (!ok) {
		err = -errno;
		EPRINTF("error writing %s: %d\n", _path, err);
		goto abort;
	}

	abort = 0;

abort:
	ok = xs_transaction_end(channel->xsh, xbt, abort);
	if (!ok) {
		err = -errno;
		if (err == -EAGAIN && !abort)
			goto again;
		EPRINTF("error ending transaction: %d\n", err);
	}

	return err;
}

static int
tapdisk_channel_trigger_reprobe(tapdisk_channel_t *channel)
{
	int err;

	/*
	 * NB. Kick the probe watch, paranoia-style. Performing an
	 * atomic test/set on the directory path. Abort if it's
	 * already gone again. Accidentally recreating the node would
	 * lead to a spurious start failure.
	 */

	err = tapdisk_channel_write_atomic(channel, channel->path, "", 0);
	if (err && err != -ENOENT)
		EPRINTF("error writing %s: %d\n", channel->pause_done_str, err);

	DPRINTF("write %s: %d\n", channel->path, err);
	return err;
}

static int
tapdisk_channel_signal_paused(tapdisk_channel_t *channel)
{
	bool ok;
	int err;

	err = tapdisk_channel_write_atomic(channel,
					   channel->pause_done_str, "", 0);
	if (err && errno != -ENOENT)
		EPRINTF("error writing %s: %d\n", channel->pause_done_str, err);

	DPRINTF("write %s: %d\n", channel->pause_done_str, err);
	return err;
}

static int
tapdisk_channel_signal_unpaused(tapdisk_channel_t *channel)
{
	bool ok;
	int err;

	ok = xs_rm(channel->xsh, XBT_NULL, channel->pause_done_str);
	err = ok ? 0 : -errno;
	if (err && err != -ENOENT)
		EPRINTF("error removing %s: %d\n", channel->pause_done_str, err);

	DPRINTF("clear %s: %d\n", channel->pause_done_str, err);
	return err;
}

static int
tapdisk_channel_receive_pause_response(tapdisk_channel_t *channel,
				       tapdisk_message_t *message)
{
	channel->state = TAPDISK_CHANNEL_PAUSED;
	return 0;
}

static int
tapdisk_channel_send_resume_request(tapdisk_channel_t *channel)
{
	int len;
	tapdisk_message_t message;

	memset(&message, 0, sizeof(tapdisk_message_t));

	len = strlen(channel->vdi_path);

	DPRINTF("resuming %s\n", channel->path);

	message.type              = TAPDISK_MESSAGE_RESUME;
	message.drivertype        = channel->drivertype;
	message.cookie            = channel->cookie;
	message.u.params.path_len = len;
	strncpy(message.u.params.path, channel->vdi_path, len);

	return tapdisk_channel_send_message(channel, &message, 2);
}

static int
tapdisk_channel_receive_resume_response(tapdisk_channel_t *channel,
					tapdisk_message_t *message)
{
	channel->state = TAPDISK_CHANNEL_RUNNING;
	return 0;
}

static int
tapdisk_channel_check_start_request(tapdisk_channel_t *channel)
{
	char *s;
	int err;

	s = xs_read(channel->xsh, XBT_NULL, channel->start_str, NULL);
	if (!s) {
		err = -errno;
		if (err != -ENOENT) {
			EPRINTF("error reading %s: %d\n",
				channel->path, err);
			goto out;
		}
		goto down;
	}

	err = 0;
	if (!strcmp(s, "start")) {
		channel->shutdown_state = TAPDISK_VBD_UP;
		channel->shutdown_force = 0;
		goto out;

	} else if (!strcmp(s, "shutdown-force")) {
		channel->shutdown_state = TAPDISK_VBD_DOWN;
		channel->shutdown_force = 1;
		goto out;

	} else if (strcmp(s, "shutdown-normal")) {
		EPRINTF("%s: invalid request '%s'", channel->path, s);
		err = -EINVAL;
		goto out;
	}

down:
	channel->shutdown_state = TAPDISK_VBD_DOWN;
	channel->shutdown_force = 0;
out:
	DPRINTF("%s: got tapdisk-request '%s', shutdown state %s (%s): %d\n",
		channel->path, s,
		tapdisk_channel_shutdown_state_name(channel->shutdown_state),
		channel->shutdown_force ? "force" : "normal", err);
	free(s);
	return err;
}

static void
tapdisk_channel_shutdown_event(struct xs_handle *xsh,
			       struct xenbus_watch *watch, const char *path)
{
	tapdisk_channel_t *channel;
	int err;

	channel = watch->data;

	DPRINTF("%s: got watch on %s\n", channel->path, path);

	err = tapdisk_channel_validate_watch(channel, path);
	if (err) {
		if (err == -EINVAL)
			tapdisk_channel_fatal(channel, "bad shutdown watch");
		return;
	}

	err = tapdisk_channel_check_start_request(channel);
	if (err)
		tapdisk_channel_error(channel, "shutdown event failed: %d", err);
	else
		tapdisk_channel_drive_vbd_state(channel);
}

static int
tapdisk_channel_drive_paused(tapdisk_channel_t *channel)
{
	int err;

	switch (channel->state) {
	case TAPDISK_CHANNEL_WAIT_PID:
	case TAPDISK_CHANNEL_WAIT_OPEN:
	case TAPDISK_CHANNEL_WAIT_PAUSE:
	case TAPDISK_CHANNEL_WAIT_RESUME:
	case TAPDISK_CHANNEL_WAIT_CLOSE:
		return -EAGAIN;

	case TAPDISK_CHANNEL_PID:
	case TAPDISK_CHANNEL_PAUSED:
	case TAPDISK_CHANNEL_CLOSED:
	case TAPDISK_CHANNEL_DEAD:
		return 0;

	case TAPDISK_CHANNEL_RUNNING:
		err = tapdisk_channel_send_pause_request(channel);
		if (err)
			goto fail_msg;
		return -EAGAIN;

	default:
		EPRINTF("%s: invalid channel state %d\n",
			__func__, channel->state);
		return -EINVAL;
	}

fail_msg:
	tapdisk_channel_fatal(channel, "sending message: %d", err);
	return -EIO;
}

static int
tapdisk_channel_drive_shutdown(tapdisk_channel_t *channel)
{
	int err;

	switch (channel->state) {

	case TAPDISK_CHANNEL_DEAD:
		return 0;

	case TAPDISK_CHANNEL_CLOSED:
		if (channel->shared)
			return 0;
		/* let's duely wait for a clean exit */
		return -EAGAIN;

	case TAPDISK_CHANNEL_LAUNCHED:
	case TAPDISK_CHANNEL_WAIT_PID:
	case TAPDISK_CHANNEL_WAIT_OPEN:
	case TAPDISK_CHANNEL_WAIT_PAUSE:
	case TAPDISK_CHANNEL_WAIT_RESUME:
	case TAPDISK_CHANNEL_WAIT_CLOSE:
		return -EAGAIN;

	case TAPDISK_CHANNEL_PID:
	case TAPDISK_CHANNEL_RUNNING:
	case TAPDISK_CHANNEL_PAUSED:
		if (channel->shutdown_force)
			err = tapdisk_channel_send_force_shutdown_request(channel);
		else
			err = tapdisk_channel_send_shutdown_request(channel);
		if (err)
			goto fail_msg;
		return -EAGAIN;

	default:
		EPRINTF("%s: invalid channel state %d\n",
			__func__, channel->state);
		return -EINVAL;
	}

fail_msg:
	tapdisk_channel_fatal(channel, "sending message: %d", err);
	return -EIO;
}

static int
tapdisk_channel_drive_running(tapdisk_channel_t *channel)
{
	int err;

	switch (channel->state) {
	case TAPDISK_CHANNEL_DEAD:
	case TAPDISK_CHANNEL_CLOSED:
		err = tapdisk_channel_connect(channel);
		if (err) {
			tapdisk_channel_fatal(channel, "failed connect: %d", err);
			return err;
		}
		return -EAGAIN;

	case TAPDISK_CHANNEL_LAUNCHED:
	case TAPDISK_CHANNEL_WAIT_PID:
	case TAPDISK_CHANNEL_WAIT_OPEN:
	case TAPDISK_CHANNEL_WAIT_PAUSE:
	case TAPDISK_CHANNEL_WAIT_RESUME:
	case TAPDISK_CHANNEL_WAIT_CLOSE:
		return -EAGAIN;

	case TAPDISK_CHANNEL_PID:
		err = tapdisk_channel_send_open_request(channel);
		if (err)
			goto fail_msg;
		return -EAGAIN;

	case TAPDISK_CHANNEL_RUNNING:
		return 0;

	case TAPDISK_CHANNEL_PAUSED:
		err = tapdisk_channel_send_resume_request(channel);
		if (err)
			goto fail_msg;
		return -EAGAIN;

	default:
		EPRINTF("%s: invalid channel state %d\n",
			__func__, channel->state);
		return -EINVAL;
	}

fail_msg:
	tapdisk_channel_fatal(channel, "sending message: %d", err);
	return -EIO;
}

static channel_state_t
tapdisk_channel_map_vbd_state(tapdisk_channel_t *channel)
{
	channel_state_t next;

	switch (channel->shutdown_state) {
	case TAPDISK_VBD_DOWN:
		return TAPDISK_CHANNEL_CLOSED;

	case TAPDISK_VBD_UP:
		switch (channel->vbd_state) {
		case TAPDISK_VBD_UNPAUSED:
			return TAPDISK_CHANNEL_RUNNING;

		case TAPDISK_VBD_PAUSING:
		case TAPDISK_VBD_PAUSED:
			return TAPDISK_CHANNEL_PAUSED;

		case TAPDISK_VBD_BROKEN:
		case TAPDISK_VBD_DEAD:
		case TAPDISK_VBD_RECYCLED:
			return TAPDISK_CHANNEL_CLOSED;

		default:
			EPRINTF("%s: invalid vbd state %d\n",
				__func__, channel->vbd_state);
			return -EINVAL;
		}
		break;
	default:
		EPRINTF("%s: invalid shutdown state %d\n",
			__func__, channel->shutdown_state);
		return -EINVAL;
	}
}

void
tapdisk_channel_drive_vbd_state(tapdisk_channel_t *channel)
{
	channel_state_t next;

	next = tapdisk_channel_map_vbd_state(channel);
	DPRINTF("driving channel state %s, vbd %s, %s to %s (%d)\n",
		tapdisk_channel_state_name(channel->state),
		tapdisk_channel_shutdown_state_name(channel->shutdown_state),
		tapdisk_channel_vbd_state_name(channel->vbd_state),
		tapdisk_channel_state_name(next), next);
	if (next < 0)
		return;

	if (channel->state != next) {
		int err = 0;

		switch (next) {
		case TAPDISK_CHANNEL_RUNNING:
			err = tapdisk_channel_drive_running(channel);
			break;
		case TAPDISK_CHANNEL_PAUSED:
			err = tapdisk_channel_drive_paused(channel);
			break;
		case TAPDISK_CHANNEL_CLOSED:
			err = tapdisk_channel_drive_shutdown(channel);
			break;
		default:
			EPRINTF("%s: invalid target state %d\n", __func__, next);
			err = -EINVAL;
			break;
		}
		if (err)
			/* -EAGAIN: not there yet */
			return;
	}

	switch (channel->vbd_state) {
	case TAPDISK_VBD_UNPAUSED:
	case TAPDISK_VBD_PAUSED:
	case TAPDISK_VBD_BROKEN:
		break;

	case TAPDISK_VBD_PAUSING:
		tapdisk_channel_enter_vbd_state(channel, TAPDISK_VBD_PAUSED);
		tapdisk_channel_signal_paused(channel);
		break;

	case TAPDISK_VBD_RECYCLED:
		tapdisk_channel_trigger_reprobe(channel);
		/* then destroy */
	case TAPDISK_VBD_DEAD:
		tapdisk_channel_destroy(channel);
	}
}

static int
tapdisk_channel_check_pause_request(tapdisk_channel_t *channel)
{
	int pause, err = 0;

	pause = xs_exists(channel->xsh, channel->pause_str);
	if (pause)
		tapdisk_channel_enter_vbd_state(channel, TAPDISK_VBD_PAUSING);
	else {
		err = tapdisk_channel_refresh_params(channel);
		if (!err)
			err = tapdisk_channel_enter_vbd_state(channel, TAPDISK_VBD_UNPAUSED);
		if (!err)
			err = tapdisk_channel_signal_unpaused(channel);
	}

	return err;
}

static void
tapdisk_channel_pause_event(struct xs_handle *xsh,
			    struct xenbus_watch *watch, const char *path)
{
	int err, count;
	tapdisk_channel_t *channel;

	channel = watch->data;

	DPRINTF("%s: got watch on %s\n", channel->path, path);

	err = tapdisk_channel_validate_watch(channel, path);
	if (err) {
		if (err == -EINVAL)
			tapdisk_channel_fatal(channel, "bad pause watch");
		return;
	}

	err = tapdisk_channel_check_pause_request(channel);
	if (err)
		tapdisk_channel_error(channel, "pause event failed: %d", err);
	else
		tapdisk_channel_drive_vbd_state(channel);
}

static int
tapdisk_channel_open_control_socket(char *devname)
{
	int err, fd;
	fd_set socks;
	struct timeval timeout;

	err = mkdir(BLKTAP_CTRL_DIR, 0755);
	if (err == -1 && errno != EEXIST) {
		EPRINTF("Failure creating %s directory: %d\n",
			BLKTAP_CTRL_DIR, errno);
		return -errno;
	}

	err = mkfifo(devname, S_IRWXU | S_IRWXG | S_IRWXO);
	if (err) {
		if (errno == EEXIST) {
			/*
			 * Remove fifo since it may have data from
			 * it's previous use --- earlier invocation
			 * of tapdisk may not have read all messages.
			 */
			err = unlink(devname);
			if (err) {
				EPRINTF("ERROR: unlink(%s) failed (%d)\n",
					devname, errno);
				return -errno;
			}

			err = mkfifo(devname, S_IRWXU | S_IRWXG | S_IRWXO);
		}

		if (err) {
			EPRINTF("ERROR: pipe failed (%d)\n", errno);
			return -errno;
		}
	}

	fd = open(devname, O_RDWR | O_NONBLOCK);
	if (fd == -1) {
		EPRINTF("Failed to open %s\n", devname);
		return -errno;
	}

	return fd;
}

static int
tapdisk_channel_get_device_number(tapdisk_channel_t *channel)
{
	char *devname;
	domid_translate_t tr;
	int major, minor, err;

	tr.domid = channel->domid;
        tr.busid = channel->busid;

	minor = ioctl(channel->blktap_fd, BLKTAP_IOCTL_NEWINTF, tr);
	if (minor <= 0 || minor > MAX_TAP_DEV) {
		EPRINTF("invalid dev id: %d\n", minor);
		return -EINVAL;
	}

	major = ioctl(channel->blktap_fd, BLKTAP_IOCTL_MAJOR, minor);
	if (major < 0) {
		EPRINTF("invalid major id: %d\n", major);
		return -EINVAL;
	}

	err = asprintf(&devname, "%s/%s%d",
		       BLKTAP_DEV_DIR, BLKTAP_DEV_NAME, minor);
	if (err == -1) {
		EPRINTF("get_new_dev: malloc failed\n");
		return -ENOMEM;
	}

	err = make_blktap_device(devname, major, minor, S_IFCHR | 0600);
	free(devname);

	if (err)
		return err;

	DPRINTF("Received device id %d and major %d, "
		"sent domid %d and be_id %d\n",
		minor, major, tr.domid, tr.busid);

	channel->major = major;
	channel->minor = minor;

	return 0;
}

static int
tapdisk_channel_start_process(tapdisk_channel_t *channel,
			      char *write_dev, char *read_dev)
{
	pid_t child;
	char opt_facility[32];
	char *argv[] = { "tapdisk", opt_facility, write_dev, read_dev, NULL };
	const char *tapdisk;
	int i;

	if ((child = fork()) == -1)
		return -errno;

	if (child)
		return child;

	for (i = 0; i < sysconf(_SC_OPEN_MAX) ; i++)
		if (i != STDIN_FILENO &&
		    i != STDOUT_FILENO &&
		    i != STDERR_FILENO)
			close(i);

	snprintf(opt_facility, sizeof(opt_facility),
		 "-l%d", tapdisk_daemon_log_facility);

	tapdisk = getenv("TAPDISK");
	if (!tapdisk)
		tapdisk = argv[0];

	execvp(tapdisk, argv);

	PERROR("execvp");
	_exit(1);
}

static void
tapdisk_channel_close_tapdisk(tapdisk_channel_t *channel)
{
	if (channel->read_fd >= 0) {
		close(channel->read_fd);
		channel->read_fd = -1;
	}

	if (channel->write_fd >= 0) {
		close(channel->write_fd);
		channel->write_fd = -1;
	}
}

static int
tapdisk_channel_launch_tapdisk(tapdisk_channel_t *channel)
{
	int err;
	char *read_dev, *write_dev;

	read_dev          = NULL;
	write_dev         = NULL;

	err = tapdisk_channel_get_device_number(channel);
	if (err)
		return err;

	err = asprintf(&write_dev,
		       "%s/tapctrlwrite%d", BLKTAP_CTRL_DIR, channel->minor);
	if (err == -1) {
		err = -ENOMEM;
		write_dev = NULL;
		goto fail;
	}

	err = asprintf(&read_dev,
		       "%s/tapctrlread%d", BLKTAP_CTRL_DIR, channel->minor);
	if (err == -1) {
		err = -ENOMEM;
		read_dev = NULL;
		goto fail;
	}

	channel->write_fd = tapdisk_channel_open_control_socket(write_dev);
	if (channel->write_fd < 0) {
		err = channel->write_fd;
		channel->write_fd = -1;
		goto fail;
	}

	channel->read_fd = tapdisk_channel_open_control_socket(read_dev);
	if (channel->read_fd < 0) {
		err = channel->read_fd;
		channel->read_fd = -1;
		goto fail;
	}

	channel->tapdisk_pid = 
		tapdisk_channel_start_process(channel, write_dev, read_dev);
	if (channel->tapdisk_pid < 0) {
		err = channel->tapdisk_pid;
		channel->tapdisk_pid = -1;
		goto fail;
	}

	channel->channel_id = channel->write_fd;

	free(read_dev);
	free(write_dev);

	DPRINTF("process launched, channel = %d:%d\n",
		channel->channel_id, channel->cookie);

	channel->state = TAPDISK_CHANNEL_LAUNCHED;
	return tapdisk_channel_send_pid_request(channel);

fail:
	free(read_dev);
	free(write_dev);
	tapdisk_channel_close_tapdisk(channel);
	return err;
}

static int
tapdisk_channel_connect(tapdisk_channel_t *channel)
{
	int err;

	tapdisk_daemon_maybe_clone_channel(channel);
	if (channel->tapdisk_pid)
		channel->state = TAPDISK_CHANNEL_PID;
	else
		return tapdisk_channel_launch_tapdisk(channel);

	DPRINTF("%s: process exists: %d, channel = %d:%d\n",
		channel->path, channel->tapdisk_pid,
		channel->channel_id, channel->cookie);

	err = tapdisk_channel_get_device_number(channel);
	if (err)
		return err;

	return tapdisk_channel_send_pid_request(channel);
}

static void
tapdisk_channel_uninit(tapdisk_channel_t *channel)
{
	free(channel->uuid_str);
	channel->uuid_str = NULL;

	free(channel->start_str);
	channel->start_str = NULL;

	free(channel->pause_str);
	channel->pause_str = NULL;

	free(channel->pause_done_str);
	channel->pause_done_str = NULL;

	channel->share_tapdisk_str = NULL;
}

static int
tapdisk_channel_init(tapdisk_channel_t *channel)
{
	int err;

	channel->uuid_str          = NULL;
	channel->pause_str         = NULL;
	channel->pause_done_str    = NULL;
	channel->start_str         = NULL;
	channel->share_tapdisk_str = NULL;

	err = asprintf(&channel->uuid_str,
		       "%s/tapdisk-uuid", channel->path);
	if (err == -1) {
		channel->uuid_str = NULL;
		goto fail;
	}

	err = asprintf(&channel->start_str,
		       "%s/tapdisk-request", channel->path);
	if (err == -1) {
		channel->start_str = NULL;
		goto fail;
	}

	err = asprintf(&channel->pause_str, "%s/pause", channel->path);
	if (err == -1) {
		channel->pause_str = NULL;
		goto fail;
	}

	err = asprintf(&channel->pause_done_str,
		       "%s/pause-done", channel->path);
	if (err == -1) {
		channel->pause_done_str = NULL;
		goto fail;
	}

	channel->share_tapdisk_str = "/local/domain/0/tapdisk/share-tapdisks";

	return 0;

fail:
	tapdisk_channel_uninit(channel);
	return -ENOMEM;
}

static void
tapdisk_channel_clear_watches(tapdisk_channel_t *channel)
{
	if (channel->start_watch.node) {
		unregister_xenbus_watch(channel->xsh, &channel->start_watch);
		channel->start_watch.node    = NULL;
	}

	if (channel->pause_watch.node) {
		unregister_xenbus_watch(channel->xsh, &channel->pause_watch);
		channel->pause_watch.node    = NULL;
	}
}

static int
tapdisk_channel_set_watches(tapdisk_channel_t *channel)
{
	int err;

	/* watch for start/shutdown events */
	channel->start_watch.node            = channel->start_str;
	channel->start_watch.callback        = tapdisk_channel_shutdown_event;
	channel->start_watch.data            = channel;
	err = register_xenbus_watch(channel->xsh, &channel->start_watch);
	if (err) {
		channel->start_watch.node    = NULL;
		goto fail;
	}

	/* watch for pause events */
	channel->pause_watch.node            = channel->pause_str;
	channel->pause_watch.callback        = tapdisk_channel_pause_event;
	channel->pause_watch.data            = channel;
	err = register_xenbus_watch(channel->xsh, &channel->pause_watch);
	if (err) {
		channel->pause_watch.node    = NULL;
		goto fail;
	}

	return 0;

fail:
	tapdisk_channel_clear_watches(channel);
	return err;
}

static void
tapdisk_channel_get_storage_type(tapdisk_channel_t *channel)
{
	int err, type;
	unsigned int len;
	char *path, *stype;

	channel->storage = TAPDISK_STORAGE_TYPE_DEFAULT;

	err = asprintf(&path, "%s/sm-data/storage-type", channel->path);
	if (err == -1)
		return;

	stype = xs_read(channel->xsh, XBT_NULL, path, &len);
	if (!stype)
		goto out;
	else if (!strcmp(stype, "nfs"))
		channel->storage = TAPDISK_STORAGE_TYPE_NFS;
	else if (!strcmp(stype, "ext"))
		channel->storage = TAPDISK_STORAGE_TYPE_EXT;
	else if (!strcmp(stype, "lvm"))
		channel->storage = TAPDISK_STORAGE_TYPE_LVM;

out:
	free(path);
	free(stype);
}


static int
tapdisk_channel_parse_params(tapdisk_channel_t *channel)
{
	int i, size, err;
	unsigned int len;
	char *ptr, *path, handle[10];
	char *vdi_type;
	char *vtype;

	path = channel->params;
	size = sizeof(dtypes) / sizeof(disk_info_t *);

	if (strlen(path) + 1 >= TAPDISK_MESSAGE_MAX_PATH_LENGTH)
		goto fail;

	ptr = strchr(path, ':');
	if (!ptr)
		goto fail;

	channel->vdi_path = ptr + 1;
	memcpy(handle, path, (ptr - path));
	ptr  = handle + (ptr - path);
	*ptr = '\0';

	err = asprintf(&vdi_type, "%s/sm-data/vdi-type", channel->path);
	if (err == -1)
		goto fail;

	if (xs_exists(channel->xsh, vdi_type)) {
		vtype = xs_read(channel->xsh, XBT_NULL, vdi_type, &len);
		free(vdi_type);
		if (!vtype)
			goto fail;
		if (len >= sizeof(handle) - 1) {
			free(vtype);
			goto fail;
		}
		sprintf(handle, "%s", vtype);
		free(vtype);
	}

	for (i = 0; i < size; i++) {
		if (strncmp(handle, dtypes[i]->handle, (ptr - path)))
			continue;

		if (dtypes[i]->idnum == -1)
			goto fail;

		channel->drivertype = dtypes[i]->idnum;
		return 0;
	}

fail:
	EPRINTF("%s: invalid blktap params: %s\n",
		channel->path, channel->params);
	channel->vdi_path = NULL;
	return -EINVAL;
}


static void
tapdisk_channel_release_info(tapdisk_channel_t *channel)
{
	free(channel->params);
	channel->params = NULL;

	free(channel->frontpath);
	channel->frontpath = NULL;
}

static int
tapdisk_channel_gather_info(tapdisk_channel_t *channel)
{
	int err;

	err = xs_gather(channel->xsh, channel->path,
			"frontend", NULL, &channel->frontpath,
			"params", NULL, &channel->params,
			"mode", "%c", &channel->mode, NULL);
	if (err) {
		EPRINTF("could not find device info: %d\n", err);
		goto fail;
	}

	err = tapdisk_channel_parse_params(channel);
	if (err)
		goto fail;

	tapdisk_channel_get_storage_type(channel);

	return 0;

fail:
	tapdisk_channel_release_info(channel);
	return err;
}

static int
tapdisk_channel_refresh_params(tapdisk_channel_t *channel)
{
	int err;

	free(channel->params);
	channel->params   = NULL;
	channel->vdi_path = NULL;

	err = xs_gather(channel->xsh, channel->path,
			"params", NULL, &channel->params, NULL);
	if (err) {
		EPRINTF("failure re-reading params: %d\n", err);
		channel->params = NULL;
		return err;
	}

	return tapdisk_channel_parse_params(channel);
}

static void
tapdisk_channel_destroy(tapdisk_channel_t *channel)
{
	DPRINTF("destroying channel %d:%d, state %s\n",
		channel->channel_id, channel->cookie,
		tapdisk_channel_state_name(channel->state));

	tapdisk_channel_clear_watches(channel);
	tapdisk_daemon_close_channel(channel);
	tapdisk_channel_release_info(channel);
	tapdisk_channel_uninit(channel);
	free(channel->path);
	free(channel);
}

int
tapdisk_channel_open(tapdisk_channel_t **_channel,
		     const char *path, struct xs_handle *xsh,
		     int blktap_fd, uint16_t cookie,
		     int domid, int busid)
{
	int err;
	char *msg;
	tapdisk_channel_t *channel;

	msg       = NULL;
	*_channel = NULL;

	channel = calloc(1, sizeof(tapdisk_channel_t));
	if (!channel)
		return -ENOMEM;

	channel->xsh       = xsh;
	channel->blktap_fd = blktap_fd;
	channel->cookie    = cookie;
	channel->domid     = domid;
	channel->busid     = busid;
	channel->state     = TAPDISK_CHANNEL_DEAD;
	channel->read_fd   = -1;
	channel->write_fd  = -1;

	INIT_LIST_HEAD(&channel->list);

	channel->path = strdup(path);
	if (!channel->path) {
		err = -ENOMEM;
		goto fail;
	}

	err = tapdisk_channel_init(channel);
	if (err) {
		msg = "allocating device";
		goto fail;
	}

	err = tapdisk_channel_check_uuid(channel);
	if (err) {
		msg = "checking uuid";
		goto fail;
	}

	err = tapdisk_channel_gather_info(channel);
	if (err) {
		msg = "gathering parameters";
		goto fail;
	}

	err = tapdisk_channel_set_watches(channel);
	if (err) {
		msg = "registering xenstore watches";
		goto fail;
	}

	err = tapdisk_channel_check_start_request(channel);
	if (err && err != -ENOENT) {
		msg = "initializing shutdown state";
		goto fail;
	}

	err = tapdisk_channel_check_pause_request(channel);
	if (err) {
		msg = "initializing pause state";
		goto fail;
	}

	*_channel = channel;
	return 0;

fail:
	tapdisk_channel_fatal(channel, "%s: %d", (msg ? : "failure"), err);
	return err;
}

void
tapdisk_channel_reap(tapdisk_channel_t *channel, int status)
{
	const char *chn_state, *vbd_state, *krn_state;

	chn_state = tapdisk_channel_state_name(channel->state);
	vbd_state = tapdisk_channel_vbd_state_name(channel->vbd_state);
	krn_state = tapdisk_channel_shutdown_state_name(channel->shutdown_state);

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		tapdisk_channel_fatal(channel,
				      "tapdisk died with status %d,"
				      " channel state %s, vbd %s, %s",
				      WEXITSTATUS(status),
				      chn_state, vbd_state, krn_state);
	} else if (WIFSIGNALED(status)) {
		tapdisk_channel_fatal(channel,
				      "tapdisk killed by signal %d,"
				      " channel state %s, vbd %s, %s",
				      WTERMSIG(status),
				      chn_state, vbd_state, krn_state);
	} else {
		DPRINTF("tapdisk exit, status %x,"
			" channel state %s, vbd %s, %s\n", status,
			chn_state, vbd_state, krn_state);
	}

	tapdisk_channel_close_tapdisk(channel);
	channel->state = TAPDISK_CHANNEL_DEAD;

	/* NB. we're in VBD_BROKEN state if we didn't exit properly,
	   implicitly avoiding an unwanted restart */
	tapdisk_channel_drive_vbd_state(channel);
}

int
tapdisk_channel_receive_message(tapdisk_channel_t *c, tapdisk_message_t *m)
{
	int err;

	err = tapdisk_channel_validate_message(c, m);
	if (err)
		goto fail;

	switch (m->type) {
	case TAPDISK_MESSAGE_PID_RSP:
		err = tapdisk_channel_receive_pid_response(c, m);
		break;

	case TAPDISK_MESSAGE_OPEN_RSP:
		err = tapdisk_channel_receive_open_response(c, m);
		break;

	case TAPDISK_MESSAGE_PAUSE_RSP:
		err = tapdisk_channel_receive_pause_response(c, m);
		break;

	case TAPDISK_MESSAGE_RESUME_RSP:
		err = tapdisk_channel_receive_resume_response(c, m);
		break;

	case TAPDISK_MESSAGE_CLOSE_RSP:
		err = tapdisk_channel_receive_shutdown_response(c, m);
		break;

	case TAPDISK_MESSAGE_RUNTIME_ERROR:
		err = tapdisk_channel_receive_runtime_error(c, m);
		break;

	default:
	fail:
		tapdisk_channel_fatal(c, "received unexpected message %s in state %d",
				      tapdisk_message_name(m->type), c->state);
		return -EINVAL;
	}

	tapdisk_channel_drive_vbd_state(c);
	return 0;
}
