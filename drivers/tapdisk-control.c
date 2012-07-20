/*
 * Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "list.h"
#include "tapdisk.h"
#include "tapdisk-vbd.h"
#include "tapdisk-blktap.h"
#include "tapdisk-utils.h"
#include "tapdisk-server.h"
#include "tapdisk-message.h"
#include "tapdisk-disktype.h"
#include "tapdisk-stats.h"
#include "tapdisk-control.h"
#include "tapdisk-nbdserver.h"

#define TD_CTL_MAX_CONNECTIONS  10
#define TD_CTL_SOCK_BACKLOG     32
#define TD_CTL_RECV_TIMEOUT     10
#define TD_CTL_SEND_TIMEOUT     10
#define TD_CTL_SEND_BUFSZ       ((size_t)4096)

#define DBG(_f, _a...)             tlog_syslog(TLOG_DBG, _f, ##_a)
#define ERR(err, _f, _a...)        tlog_error(err, _f, ##_a)
#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "control: " _f, ##_a)

#define ASSERT(_p)							\
	if (!(_p)) {							\
		EPRINTF("%s:%d: FAILED ASSERTION: '%s'\n",		\
			__FILE__, __LINE__, #_p);			\
		td_panic();						\
	}

#define WARN_ON(_p)							\
	if (_p) {							\
		EPRINTF("%s:%d: WARNING: '%s'\n",			\
			__FILE__, __LINE__, #_p);			\
	}

struct tapdisk_ctl_conn {
	int             fd;

	struct {
		void           *buf;
		size_t          bufsz;
		int             event_id;
		int             done;

		void           *prod;
		void           *cons;
	} out;

	struct {
		int             event_id;
		int             busy;
	} in;

	struct tapdisk_control_info *info;
};

#define TAPDISK_MSG_REENTER    (1<<0) /* non-blocking, idempotent */
#define TAPDISK_MSG_VERBOSE    (1<<1) /* tell syslog about it */

struct tapdisk_control_info {
	void (*handler)(struct tapdisk_ctl_conn *, tapdisk_message_t *);
	int flags;
};

struct tapdisk_control {
	char              *path;
	int                uuid;
	int                socket;
	int                event_id;
	int                busy;

	int                n_conn;
	struct tapdisk_ctl_conn __conn[TD_CTL_MAX_CONNECTIONS];
	struct tapdisk_ctl_conn *conn[TD_CTL_MAX_CONNECTIONS];
};

static struct tapdisk_control td_control;

static inline size_t
page_align(size_t size)
{
	size_t page_size = sysconf(_SC_PAGE_SIZE);
	return (size + page_size - 1) & ~(page_size - 1);
}

static void
tapdisk_ctl_conn_uninit(struct tapdisk_ctl_conn *conn)
{
	if (conn->out.buf) {
		free(conn->out.buf);
		conn->out.buf = NULL;
	}
}

static int
tapdisk_ctl_conn_init(struct tapdisk_ctl_conn *conn, size_t bufsz)
{
	int err;

	memset(conn, 0, sizeof(*conn));
	conn->out.event_id = -1;
	conn->in.event_id  = -1;

	conn->out.buf = malloc(bufsz);
	if (!conn->out.buf) {
		err = -ENOMEM;
		goto fail;
	}
	conn->out.bufsz = page_align(bufsz);

	return 0;

fail:
	tapdisk_ctl_conn_uninit(conn);
	return err;
}

static int
tapdisk_ctl_conn_connected(struct tapdisk_ctl_conn *conn)
{
	return conn->fd >= 1;
}

static void
tapdisk_ctl_conn_free(struct tapdisk_ctl_conn *conn)
{
	struct tapdisk_ctl_conn *prev, *next;
	int i;

	i = --td_control.n_conn;
	/* NB. bubble the freed connection off the active list. */
	prev = conn;
	do {
		ASSERT(i >= 0);
		next = td_control.conn[i];
		td_control.conn[i] = prev;
		prev = next;
		i--;
	} while (next != conn);
}

static void
tapdisk_ctl_conn_close(struct tapdisk_ctl_conn *conn)
{
	if (conn->out.event_id >= 0) {
		tapdisk_server_unregister_event(conn->out.event_id);
		conn->out.event_id = -1;
	}

	if (conn->fd >= 0) {
		close(conn->fd);
		conn->fd = -1;

		tapdisk_ctl_conn_free(conn);
		tapdisk_server_mask_event(td_control.event_id, 0);
	}
}

static void
tapdisk_ctl_conn_mask_out(struct tapdisk_ctl_conn *conn)
{
	tapdisk_server_mask_event(conn->out.event_id, 1);
}

static void
tapdisk_ctl_conn_unmask_out(struct tapdisk_ctl_conn *conn)
{
	tapdisk_server_mask_event(conn->out.event_id, 0);
}

static ssize_t
tapdisk_ctl_conn_send_buf(struct tapdisk_ctl_conn *conn)
{
	ssize_t size;

	size = conn->out.prod - conn->out.cons;
	if (!size)
		return 0;

	size = send(conn->fd, conn->out.cons, size, MSG_DONTWAIT);
	if (size < 0)
		return -errno;

	conn->out.cons += size;

	return size;
}

static void
tapdisk_ctl_conn_send_event(event_id_t id, char mode, void *private)
{
	struct tapdisk_ctl_conn *conn = private;
	ssize_t rv;

	do {
		rv = tapdisk_ctl_conn_send_buf(conn);
	} while (rv > 0);

	if (rv == -EAGAIN)
		return;

	if (rv < 0)
		ERR(rv, "failure sending message at offset %td/%td\n",
		    conn->out.cons - conn->out.buf,
		    conn->out.prod - conn->out.buf);

	if (rv || conn->out.done || mode & SCHEDULER_POLL_TIMEOUT)
		tapdisk_ctl_conn_close(conn);
	else
		tapdisk_ctl_conn_mask_out(conn);
}

/*
 * NB. the control interface is still not properly integrated into the
 * server, therefore neither the scheduler. After the last close, the
 * server will exit but we still have a pending close response in the
 * output buffer.
 */
static void
tapdisk_ctl_conn_drain(struct tapdisk_ctl_conn *conn)
{
	struct timeval tv = { .tv_sec = TD_CTL_SEND_TIMEOUT,
			      .tv_usec = 0 };
	fd_set wfds;
	int n, mode;

	ASSERT(conn->out.done);
	ASSERT(conn->fd >= 0);

	while (tapdisk_ctl_conn_connected(conn)) {
		FD_ZERO(&wfds);
		FD_SET(conn->fd, &wfds);

		n = select(conn->fd + 1, NULL, &wfds, NULL, &tv);
		if (n < 0)
			break;

		if (n)
			mode = SCHEDULER_POLL_WRITE_FD;
		else
			mode = SCHEDULER_POLL_TIMEOUT;

		tapdisk_ctl_conn_send_event(conn->out.event_id, mode, conn);
	}
}


struct tapdisk_ctl_conn *
tapdisk_ctl_conn_open(int fd)
{
	struct tapdisk_ctl_conn *conn;

	if (td_control.n_conn >= TD_CTL_MAX_CONNECTIONS)
		return NULL;

	conn = td_control.conn[td_control.n_conn++];

	conn->out.event_id =
		tapdisk_server_register_event(SCHEDULER_POLL_WRITE_FD,
					      fd, TD_CTL_SEND_TIMEOUT,
					      tapdisk_ctl_conn_send_event,
					      conn);
	if (conn->out.event_id < 0)
		return NULL;

	conn->fd       = fd;
	conn->out.prod = conn->out.buf;
	conn->out.cons = conn->out.buf;

	tapdisk_ctl_conn_mask_out(conn);

	if (td_control.n_conn >= TD_CTL_MAX_CONNECTIONS)
		tapdisk_server_mask_event(td_control.event_id, 1);

	return conn;
}

static size_t
tapdisk_ctl_conn_write(struct tapdisk_ctl_conn *conn, void *buf, size_t size)
{
	size_t rest;

	rest = conn->out.buf + conn->out.bufsz - conn->out.prod;
	if (rest < size)
		size = rest;
	if (!size)
		return 0;

	memcpy(conn->out.prod, buf, size);
	conn->out.prod += size;
	tapdisk_ctl_conn_unmask_out(conn);

	return size;
}

static void
tapdisk_ctl_conn_release(struct tapdisk_ctl_conn *conn)
{
	conn->out.done = 1;

	if (conn->out.prod == conn->out.cons)
		tapdisk_ctl_conn_close(conn);
}

static void
tapdisk_control_initialize(void)
{
	struct tapdisk_ctl_conn *conn;
	int i;

	td_control.socket   = -1;
	td_control.event_id = -1;

	signal(SIGPIPE, SIG_IGN);

	for (i = 0; i < TD_CTL_MAX_CONNECTIONS; i++) {
		conn = &td_control.__conn[i];
		tapdisk_ctl_conn_init(conn, TD_CTL_SEND_BUFSZ);
		td_control.conn[i] = conn;
	}

	td_control.n_conn = 0;

	DPRINTF("tapdisk-control: init, %d x %zuk buffers\n",
		TD_CTL_MAX_CONNECTIONS, TD_CTL_SEND_BUFSZ >> 10);
}

void
tapdisk_control_close(void)
{
	struct tapdisk_ctl_conn *conn;
	int i;

	DPRINTF("tapdisk-control: draining %d connections\n",
		td_control.n_conn);

	while (td_control.n_conn) {
		conn = td_control.conn[td_control.n_conn-1];
		tapdisk_ctl_conn_drain(conn);
	}

	for (i = 0; i < TD_CTL_MAX_CONNECTIONS; i++) {
		conn = &td_control.__conn[i];
		tapdisk_ctl_conn_uninit(conn);
	}

	DPRINTF("tapdisk-control: done\n");

	if (td_control.path) {
		unlink(td_control.path);
		free(td_control.path);
		td_control.path = NULL;
	}

	if (td_control.socket != -1) {
		close(td_control.socket);
		td_control.socket = -1;
	}
}

static void
tapdisk_control_release_connection(struct tapdisk_ctl_conn *conn)
{
	if (conn->in.event_id) {
		tapdisk_server_unregister_event(conn->in.event_id);
		conn->in.event_id = -1;
	}

	tapdisk_ctl_conn_release(conn);
}

static void
tapdisk_control_close_connection(struct tapdisk_ctl_conn *conn)
{
	tapdisk_control_release_connection(conn);

	if (tapdisk_ctl_conn_connected(conn))
		/* NB. best effort for write/close sequences. */
		tapdisk_ctl_conn_send_buf(conn);

	tapdisk_ctl_conn_close(conn);
}


static int
tapdisk_control_read_message(int fd, tapdisk_message_t *message, int timeout)
{
	const int len = sizeof(tapdisk_message_t);
	fd_set readfds;
	int ret, offset, err = 0;
	struct timeval tv, *t;

	t      = NULL;
	offset = 0;

	if (timeout) {
		tv.tv_sec  = timeout;
		tv.tv_usec = 0;
		t = &tv;
	}

	memset(message, 0, sizeof(tapdisk_message_t));

	while (offset < len) {
		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);

		ret = select(fd + 1, &readfds, NULL, NULL, t);
		if (ret == -1)
			break;
		else if (FD_ISSET(fd, &readfds)) {
			ret = read(fd, message + offset, len - offset);
			if (ret <= 0)
				break;
			offset += ret;
		} else
			break;
	}

	if (ret < 0)
		err = -errno;
	else if (offset != len)
		err = -EIO;
	if (err)
		ERR(err, "failure reading message at offset %d/%d\n",
		    offset, len);


	return err;
}

static void
tapdisk_control_write_message(struct tapdisk_ctl_conn *conn,
			      tapdisk_message_t *message)
{
	size_t size = sizeof(*message), count;

	if (conn->info && conn->info->flags & TAPDISK_MSG_VERBOSE)
		DBG("sending '%s' message (uuid = %u)\n",
		    tapdisk_message_name(message->type), message->cookie);

	count = tapdisk_ctl_conn_write(conn, message, size);
	WARN_ON(count != size);
}

static int
tapdisk_control_validate_request(tapdisk_message_t *request)
{
	if (strnlen(request->u.params.path,
		    TAPDISK_MESSAGE_MAX_PATH_LENGTH) >=
	    TAPDISK_MESSAGE_MAX_PATH_LENGTH)
		return EINVAL;

	return 0;
}

#if 0
static void
tapdisk_control_list_minors(struct tapdisk_ctl_conn *conn,
			    tapdisk_message_t *request)
{
	int i;
	td_vbd_t *vbd;
	struct list_head *head;
	tapdisk_message_t response;

	i = 0;
	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_LIST_MINORS_RSP;
	response.cookie = request->cookie;

	head = tapdisk_server_get_all_vbds();

	list_for_each_entry(vbd, head, next) {
		td_blktap_t *tap = vbd->tap;
		if (!tap)
			continue;

		response.u.minors.list[i++] = tap->minor;
		if (i >= TAPDISK_MESSAGE_MAX_MINORS) {
			response.type = TAPDISK_MESSAGE_ERROR;
			response.u.response.error = ERANGE;
			break;
		}
	}

	response.u.minors.count = i;
	tapdisk_ctl_conn_write(conn, &response, 2);
}
#endif

static void
tapdisk_control_list(struct tapdisk_ctl_conn *conn, tapdisk_message_t *request)
{
	td_vbd_t *vbd;
	struct list_head *head;
	tapdisk_message_t response;
	int count;

	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_LIST_RSP;
	response.cookie = request->cookie;

	head = tapdisk_server_get_all_vbds();

	count = 0;
	list_for_each_entry(vbd, head, next)
		count++;

	list_for_each_entry(vbd, head, next) {
		response.u.list.count   = count--;
		response.u.list.minor   = vbd->tap ? vbd->tap->minor : -1;
		response.u.list.state   = vbd->state;
		response.u.list.path[0] = 0;

		if (vbd->name)
			strncpy(response.u.list.path, vbd->name,
				sizeof(response.u.list.path));

		tapdisk_control_write_message(conn, &response);
	}

	response.u.list.count   = count;
	response.u.list.minor   = -1;
	response.u.list.path[0] = 0;

	tapdisk_control_write_message(conn, &response);
}

static void
tapdisk_control_get_pid(struct tapdisk_ctl_conn *conn,
			tapdisk_message_t *request)
{
	tapdisk_message_t response;

	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_PID_RSP;
	response.cookie = request->cookie;
	response.u.tapdisk_pid = getpid();

	tapdisk_control_write_message(conn, &response);
}

static void
tapdisk_control_attach_vbd(struct tapdisk_ctl_conn *conn,
			   tapdisk_message_t *request)
{
	tapdisk_message_t response;
	char *devname = NULL;
	td_vbd_t *vbd;
	int minor, err;

	/*
	 * TODO: check for max vbds per process
	 */

	vbd = tapdisk_server_get_vbd(request->cookie);
	if (vbd) {
		err = -EEXIST;
		goto out;
	}

	minor = request->cookie;
	if (minor < 0) {
		err = -EINVAL;
		goto out;
	}

	vbd = tapdisk_vbd_create(minor);
	if (!vbd) {
		err = -ENOMEM;
		goto out;
	}

	err = asprintf(&devname, BLKTAP2_RING_DEVICE"%d", minor);
	if (err == -1) {
		devname = NULL;
		err = -ENOMEM;
		goto fail_vbd;
	}

	err = tapdisk_vbd_attach(vbd, devname, minor);
	if (err) {
		ERR(err, "failure attaching to %s", devname);
		goto fail_vbd;
	}

	tapdisk_server_add_vbd(vbd);

out:
	if (devname)
		free(devname);

	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_ATTACH_RSP;
	response.cookie = request->cookie;
	response.u.response.error = -err;

	tapdisk_control_write_message(conn, &response);

	return;

fail_vbd:
	tapdisk_vbd_detach(vbd);
	free(vbd);
	goto out;
}


static void
tapdisk_control_detach_vbd(struct tapdisk_ctl_conn *conn,
			   tapdisk_message_t *request)
{
	tapdisk_message_t response;
	td_vbd_t *vbd;
	int err;

	vbd = tapdisk_server_get_vbd(request->cookie);
	if (!vbd) {
		err = -EINVAL;
		goto out;
	}

	if (vbd->name) {
		err = -EBUSY;
		goto out;
	}

	tapdisk_vbd_detach(vbd);

	if (list_empty(&vbd->images)) {
		tapdisk_server_remove_vbd(vbd);
		free(vbd);
	}

	err = 0;
out:
	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_DETACH_RSP;
	response.cookie = request->cookie;
	response.u.response.error = -err;

	tapdisk_control_write_message(conn, &response);
}

static void
tapdisk_control_open_image(struct tapdisk_ctl_conn *conn,
			   tapdisk_message_t *request)
{
	int err;
	td_vbd_t *vbd;
	td_flag_t flags;
	tapdisk_message_t response;
	td_disk_info_t info;

	vbd = tapdisk_server_get_vbd(request->cookie);
	if (!vbd) {
		err = -EINVAL;
		goto out;
	}

	if (!vbd->tap) {
		err = -EINVAL;
		goto out;
	}

	if (vbd->name) {
		err = -EALREADY;
		goto out;
	}

	flags = 0;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_RDONLY)
		flags |= TD_OPEN_RDONLY;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_SHARED)
		flags |= TD_OPEN_SHAREABLE;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_ADD_CACHE)
		flags |= TD_OPEN_ADD_CACHE;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_VHD_INDEX)
		flags |= TD_OPEN_VHD_INDEX;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_LOG_DIRTY)
		flags |= TD_OPEN_LOG_DIRTY;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_ADD_LCACHE)
		flags |= TD_OPEN_LOCAL_CACHE;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_REUSE_PRT)
		flags |= TD_OPEN_REUSE_PARENT;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_STANDBY)
		flags |= TD_OPEN_STANDBY;
	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_SECONDARY) {
		char *name = strdup(request->u.params.secondary);
		if (!name) {
			err = -errno;
			goto out;
		}
		vbd->secondary_name = name;
		flags |= TD_OPEN_SECONDARY;
	}

	err = tapdisk_vbd_open_vdi(vbd, request->u.params.path, flags,
				   request->u.params.prt_devnum);
	if (err)
		goto out;

	err = tapdisk_vbd_get_disk_info(vbd, &info);
	if (err)
		goto fail_close;

	err = tapdisk_blktap_create_device(vbd->tap, &info,
					   !!(flags & TD_OPEN_RDONLY));
	if (err && err != -EEXIST) {
		err = -errno;
		EPRINTF("create device failed: %d\n", err);
		goto fail_close;
	}

	/* For now, lets do this automatically on all 'open' calls
	   In the future, we'll probably want a separate call to
	   start the NBD server */
	err = tapdisk_vbd_start_nbdserver(vbd);

	if (err) {
		EPRINTF("failed to start nbdserver: %d\n",err);
		goto fail_close;
	}

	err = 0;

out:
	memset(&response, 0, sizeof(response));
	response.cookie = request->cookie;

	if (err) {
		response.type                = TAPDISK_MESSAGE_ERROR;
		response.u.response.error    = -err;
	} else {
		response.u.image.sectors     = info.size;
		response.u.image.sector_size = info.sector_size;
		response.u.image.info        = info.info;
		response.type                = TAPDISK_MESSAGE_OPEN_RSP;
	}

	tapdisk_control_write_message(conn, &response);

	return;

fail_close:
	tapdisk_vbd_close_vdi(vbd);

	if (vbd->name) {
		free(vbd->name);
		vbd->name = NULL;
	}

	goto out;
}

static void
tapdisk_control_close_image(struct tapdisk_ctl_conn *conn,
			    tapdisk_message_t *request)
{
	tapdisk_message_t response;
	td_vbd_t *vbd;
	int err;

	vbd = tapdisk_server_get_vbd(request->cookie);
	if (!vbd) {
		err = -ENODEV;
		goto out;
	}

	if(vbd->nbdserver) {
	  tapdisk_nbdserver_pause(vbd->nbdserver);
	}

	do {
		err = tapdisk_blktap_remove_device(vbd->tap);

        if (err == -EBUSY)
            tlog_write(TLOG_WARN, "device still open\n");

		if (!err || err != -EBUSY)
			break;

		tapdisk_server_iterate();

	} while (conn->fd >= 0);

	if (err)
		ERR(err, "failure closing image\n");

	if (err == -ENOTTY) {

		while (!list_empty(&vbd->pending_requests))
			tapdisk_server_iterate();

		err = 0;
	}

	if (err)
		goto out;

	if(vbd->nbdserver) {
	  tapdisk_nbdserver_free(vbd->nbdserver);
	  vbd->nbdserver = NULL;
	}

	tapdisk_vbd_close_vdi(vbd);

	/* NB. vbd->name free should probably belong into close_vdi,
	   but the current blktap1 reopen-stuff likely depends on a
	   lifetime extended until shutdown. */
	free(vbd->name);
	vbd->name = NULL;

	if (!vbd->tap) {
		tapdisk_server_remove_vbd(vbd);
		free(vbd);
	}

out:
	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_CLOSE_RSP;
	response.cookie = request->cookie;
	response.u.response.error = -err;

	tapdisk_control_write_message(conn, &response);
}

static void
tapdisk_control_pause_vbd(struct tapdisk_ctl_conn *conn,
			  tapdisk_message_t *request)
{
	int err;
	td_vbd_t *vbd;
	tapdisk_message_t response;

	memset(&response, 0, sizeof(response));

	response.type = TAPDISK_MESSAGE_PAUSE_RSP;

	vbd = tapdisk_server_get_vbd(request->cookie);
	if (!vbd) {
		err = -EINVAL;
		goto out;
	}

	do {
		err = tapdisk_vbd_pause(vbd);

		if (!err || err != -EAGAIN)
			break;

		tapdisk_server_iterate();

	} while (conn->fd >= 0);

out:
	response.cookie = request->cookie;
	response.u.response.error = -err;
	tapdisk_control_write_message(conn, &response);
}

static void
tapdisk_control_resume_vbd(struct tapdisk_ctl_conn *conn,
			   tapdisk_message_t *request)
{
	int err;
	td_vbd_t *vbd;
	tapdisk_message_t response;
	const char *desc = NULL;

	memset(&response, 0, sizeof(response));

	response.type = TAPDISK_MESSAGE_RESUME_RSP;

	INFO("Resuming. flags=0x%08x secondary=%p\n", request->u.params.flags, request->u.params.secondary);

	vbd = tapdisk_server_get_vbd(request->cookie);
	if (!vbd) {
		err = -EINVAL;
		goto out;
	}

	if (request->u.params.flags & TAPDISK_MESSAGE_FLAG_SECONDARY) {
		char *name = strdup(request->u.params.secondary);
		if (!name) {
			err = -errno;
			goto out;
		}
		INFO("Resuming with secondary '%s'\n", name);
		vbd->secondary_name = name;
		vbd->flags |= TD_OPEN_SECONDARY;
	}

	if (request->u.params.path[0])
		desc = request->u.params.path;

	err = tapdisk_vbd_resume(vbd, desc);
out:
	response.cookie = request->cookie;
	response.u.response.error = -err;
	tapdisk_control_write_message(conn, &response);
}

static void
tapdisk_control_stats(struct tapdisk_ctl_conn *conn,
		      tapdisk_message_t *request)
{
	tapdisk_message_t response;
	td_stats_t _st, *st = &_st;
	td_vbd_t *vbd;
	size_t rv;
	void *buf;
	int new_size;

	buf = malloc(TD_CTL_SEND_BUFSZ);
	if (!buf) {
		rv = -ENOMEM;
		goto out;
	}

	tapdisk_stats_init(st, buf, TD_CTL_SEND_BUFSZ);

	if (request->cookie != (uint16_t)-1) {

		vbd = tapdisk_server_get_vbd(request->cookie);
		if (!vbd) {
			rv = -ENODEV;
			goto out;
		}

		tapdisk_vbd_stats(vbd, st);

	} else {
		struct list_head *list = tapdisk_server_get_all_vbds();

		tapdisk_stats_enter(st, '[');

		list_for_each_entry(vbd, list, next)
			tapdisk_vbd_stats(vbd, st);

		tapdisk_stats_leave(st, ']');
	}

	rv = tapdisk_stats_length(st);

	if (rv > conn->out.bufsz - sizeof(response)) {
		ASSERT(conn->out.prod == conn->out.buf);
		ASSERT(conn->out.cons == conn->out.buf);
		new_size = rv + sizeof(response);
		buf = realloc(conn->out.buf, new_size);
		if (!buf) {
			rv = -ENOMEM;
			goto out;
		}
		conn->out.buf = buf;
		conn->out.bufsz = new_size;
		conn->out.prod = buf;
		conn->out.cons = buf;
	}
	if (rv > 0) {
		memcpy(conn->out.buf + sizeof(response), st->buf, rv);
	}
out:
	free(st->buf);
	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_STATS_RSP;
	response.cookie = request->cookie;
	response.u.info.length = rv;

	tapdisk_control_write_message(conn, &response);
	if (rv > 0)
		conn->out.prod += rv;
}

struct tapdisk_control_info message_infos[] = {
	[TAPDISK_MESSAGE_PID] = {
		.handler = tapdisk_control_get_pid,
		.flags   = TAPDISK_MSG_REENTER,
	},
	[TAPDISK_MESSAGE_LIST] = {
		.handler = tapdisk_control_list,
		.flags   = TAPDISK_MSG_REENTER,
	},
	[TAPDISK_MESSAGE_ATTACH] = {
		.handler = tapdisk_control_attach_vbd,
		.flags   = TAPDISK_MSG_VERBOSE,
	},
	[TAPDISK_MESSAGE_DETACH] = {
		.handler = tapdisk_control_detach_vbd,
		.flags   = TAPDISK_MSG_VERBOSE,
	},
	[TAPDISK_MESSAGE_OPEN] = {
		.handler = tapdisk_control_open_image,
		.flags   = TAPDISK_MSG_VERBOSE,
	},
	[TAPDISK_MESSAGE_PAUSE] = {
		.handler = tapdisk_control_pause_vbd,
		.flags   = TAPDISK_MSG_VERBOSE,
	},
	[TAPDISK_MESSAGE_RESUME] = {
		.handler = tapdisk_control_resume_vbd,
		.flags   = TAPDISK_MSG_VERBOSE,
	},
	[TAPDISK_MESSAGE_CLOSE] = {
		.handler = tapdisk_control_close_image,
		.flags   = TAPDISK_MSG_VERBOSE,
	},
	[TAPDISK_MESSAGE_STATS] = {
		.handler = tapdisk_control_stats,
		.flags   = TAPDISK_MSG_REENTER,
	},
};


static void
tapdisk_control_handle_request(event_id_t id, char mode, void *private)
{
	int err, excl;
	tapdisk_message_t message, response;
	struct tapdisk_ctl_conn *conn = private;

	conn->info = NULL;

	err = tapdisk_control_read_message(conn->fd, &message, 2);
	if (err)
		goto close;

	err = tapdisk_control_validate_request(&message);
	if (err)
		goto invalid;

	if (message.type > TAPDISK_MESSAGE_EXIT)
		goto invalid;

	conn->info = &message_infos[message.type];

	if (!conn->info->handler)
		goto invalid;

	if (conn->info->flags & TAPDISK_MSG_VERBOSE)
		DBG("received '%s' message (uuid = %u)\n",
		    tapdisk_message_name(message.type), message.cookie);

	if (conn->in.busy)
		goto busy;

	excl = !(conn->info->flags & TAPDISK_MSG_REENTER);
	if (excl) {
		if (td_control.busy)
			goto busy;

		td_control.busy = 1;
	}
	conn->in.busy = 1;

	conn->info->handler(conn, &message);

	conn->in.busy = 0;
	if (excl)
		td_control.busy = 0;

	tapdisk_control_release_connection(conn);
	return;

error:
	memset(&response, 0, sizeof(response));
	response.type = TAPDISK_MESSAGE_ERROR;
	response.u.response.error = (err ? -err : EINVAL);
	tapdisk_control_write_message(conn, &response);

close:
	tapdisk_control_close_connection(conn);
	return;

busy:
	err = -EBUSY;
	ERR(err, "rejecting message '%s' while busy\n",
	    tapdisk_message_name(message.type));
	goto error;

invalid:
	err = -EINVAL;
	ERR(err, "rejecting unsupported message '%s'\n",
	    tapdisk_message_name(message.type));
	goto error;
}

static void
tapdisk_control_accept(event_id_t id, char mode, void *private)
{
	int err, fd;
	struct tapdisk_ctl_conn *conn;

	fd = accept(td_control.socket, NULL, NULL);
	if (fd == -1) {
		ERR(-errno, "failed to accept new control connection: %d\n", errno);
		return;
	}

	conn = tapdisk_ctl_conn_open(fd);
	if (!conn) {
		close(fd);
		ERR(-ENOMEM, "failed to allocate new control connection\n");
		return;
	}

	err = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					    conn->fd, TD_CTL_RECV_TIMEOUT,
					    tapdisk_control_handle_request,
					    conn);
	if (err == -1) {
		tapdisk_control_close_connection(conn);
		ERR(err, "failed to register new control event\n");
		return;
	}

	conn->in.event_id = err;
}

static int
tapdisk_control_mkdir(const char *dir)
{
	int err;
	char *ptr, *name, *start;

	err = access(dir, W_OK | R_OK);
	if (!err)
		return 0;

	name = strdup(dir);
	if (!name)
		return -ENOMEM;

	start = name;

	for (;;) {
		ptr = strchr(start + 1, '/');
		if (ptr)
			*ptr = '\0';

		err = mkdir(name, 0755);
		if (err && errno != EEXIST) {
			err = -errno;
			EPRINTF("failed to create directory %s: %d\n",
				  name, err);
			break;
		}

		if (!ptr)
			break;
		else {
			*ptr = '/';
			start = ptr + 1;
		}
	}

	free(name);
	return err;
}

static int
tapdisk_control_create_socket(char **socket_path)
{
	struct sockaddr_un saddr;
	int err;

	err = tapdisk_control_mkdir(BLKTAP2_CONTROL_DIR);
	if (err) {
		EPRINTF("failed to create directory %s: %d\n",
			BLKTAP2_CONTROL_DIR, err);
		return err;
	}

	err = asprintf(&td_control.path, "%s/%s%d",
		       BLKTAP2_CONTROL_DIR, BLKTAP2_CONTROL_SOCKET, getpid());
	if (err == -1) {
		td_control.path = NULL;
		err = (errno ? : ENOMEM);
		goto fail;
	}

	if (unlink(td_control.path) && errno != ENOENT) {
		err = errno;
		EPRINTF("failed to unlink %s: %d\n", td_control.path, errno);
		goto fail;
	}

	td_control.socket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (td_control.socket == -1) {
		err = errno;
		EPRINTF("failed to create control socket: %d\n", err);
		goto fail;
	}

	memset(&saddr, 0, sizeof(saddr));
	strncpy(saddr.sun_path, td_control.path, sizeof(saddr.sun_path));
	saddr.sun_family = AF_UNIX;

	err = bind(td_control.socket,
		   (const struct sockaddr *)&saddr, sizeof(saddr));
	if (err == -1) {
		err = errno;
		EPRINTF("failed to bind to %s: %d\n", saddr.sun_path, err);
		goto fail;
	}

	err = listen(td_control.socket, TD_CTL_SOCK_BACKLOG);
	if (err == -1) {
		err = errno;
		EPRINTF("failed to listen: %d\n", err);
		goto fail;
	}

	err = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					    td_control.socket, 0,
					    tapdisk_control_accept, NULL);
	if (err < 0) {
		EPRINTF("failed to add watch: %d\n", err);
		goto fail;
	}

	td_control.event_id = err;
	*socket_path = td_control.path;

	return 0;

fail:
	tapdisk_control_close();
	return err;
}

int
tapdisk_control_open(char **path)
{
	tapdisk_control_initialize();

	return tapdisk_control_create_socket(path);
}
