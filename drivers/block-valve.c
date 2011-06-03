/*
 * Copyright (c) 2010, Citrix Systems, Inc.
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "tapdisk.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-interface.h"

#include "block-valve.h"

typedef struct td_valve td_valve_t;
typedef struct td_valve_request td_valve_request_t;

struct td_valve_request {
	td_request_t            treq;
	int                     secs;

	struct list_head        entry;
	td_valve_t             *valve;
};

struct td_valve_stats {
	unsigned long long      stor;
	unsigned long long      forw;
};

struct td_valve {
	char                   *brname;
	unsigned long           flags;

	int                     sock;
	event_id_t              sock_id;

	event_id_t              sched_id;
	event_id_t              retry_id;

	unsigned int            cred;
	unsigned int            need;
	unsigned int            done;

	struct list_head        stor;
	struct list_head        forw;

	td_valve_request_t      reqv[MAX_REQUESTS];
	td_valve_request_t     *free[MAX_REQUESTS];
	int                     n_free;

	struct td_valve_stats   stats;
};

#define td_valve_for_each_stored_request(_req, _next, _valve)		\
	list_for_each_entry_safe(_req, _next, &(_valve)->stor, entry)

#define td_valve_for_each_forwarded_request(_req, _next, _valve)	\
	list_for_each_entry_safe(_req, _next, &(_valve)->forw, entry)

#define TD_VALVE_CONNECT_INTERVAL 2 /* s */

#define TD_VALVE_RDLIMIT  (1<<0)
#define TD_VALVE_WRLIMIT  (1<<1)
#define TD_VALVE_KILLED   (1<<31)

static void valve_schedule_retry(td_valve_t *);
static void valve_conn_receive(td_valve_t *);
static void valve_conn_request(td_valve_t *, unsigned long);
static void valve_forward_stored_requests(td_valve_t *);
static void valve_kill(td_valve_t *);

#define DBG(_f, _a...)    if (1) { tlog_syslog(TLOG_DBG, _f, ##_a); }
#define INFO(_f, _a...)   tlog_syslog(TLOG_INFO, "valve: " _f, ##_a)
#define WARN(_f, _a...)   tlog_syslog(TLOG_WARN, "WARNING: "_f " in %s:%d", \
				      ##_a, __func__, __LINE__)
#define ERR(_f, _a...)    tlog_syslog(TLOG_WARN, "ERROR: " _f " in %s:%d", \
				      ##_a, __func__, __LINE__)
#define VERR(_err, _f, _a...) tlog_syslog(TLOG_WARN,			 \
					  "ERROR: err=%d (%s), " _f ".", \
					  _err, strerror(-(_err)), ##_a)
#undef  PERROR
#define PERROR(_f, _a...) VERR(-errno, _f, ##_a)

#define BUG() do {						\
		ERR("Aborting");				\
		td_panic();					\
	} while (0)

#define BUG_ON(_cond)						\
	if (unlikely(_cond)) {					\
		ERR("(%s) = %ld", #_cond, (long)(_cond));	\
		BUG();						\
	}

#define WARN_ON(_cond) ({					\
	int __cond = _cond;					\
	if (unlikely(__cond))					\
		WARN("(%s) = %ld", #_cond, (long)(_cond));	\
	__cond;						\
})

#define ARRAY_SIZE(_a)   (sizeof(_a)/sizeof((_a)[0]))
#define TREQ_SIZE(_treq) ((unsigned int)(_treq.secs) << 9)

static td_valve_request_t *
valve_alloc_request(td_valve_t *valve)
{
	td_valve_request_t *req = NULL;

	if (valve->n_free)
		req = valve->free[--valve->n_free];

	return req;
}

static void
valve_free_request(td_valve_t *valve, td_valve_request_t *req)
{
	BUG_ON(valve->n_free >= ARRAY_SIZE(valve->free));
	list_del_init(&req->entry);
	valve->free[valve->n_free++] = req;
}

static void
__valve_sock_event(event_id_t id, char mode, void *private)
{
	td_valve_t *valve = private;

	valve_conn_receive(valve);

	valve_forward_stored_requests(valve);
}

static void
valve_set_done_pending(td_valve_t *valve)
{
	WARN_ON(valve->done == 0);
	tapdisk_server_mask_event(valve->sched_id, 0);
}

static void
valve_clear_done_pending(td_valve_t *valve)
{
	WARN_ON(valve->done != 0);
	tapdisk_server_mask_event(valve->sched_id, 1);
}

static void
__valve_sched_event(event_id_t id, char mode, void *private)
{
	td_valve_t *valve = private;

	if (likely(valve->done > 0))
		/* flush valve->done */
		valve_conn_request(valve, 0);
}

static void
valve_sock_close(td_valve_t *valve)
{
	if (valve->sock >= 0) {
		close(valve->sock);
		valve->sock = -1;
	}

	if (valve->sock_id >= 0) {
		tapdisk_server_unregister_event(valve->sock_id);
		valve->sock_id = -1;
	}

	if (valve->sched_id >= 0) {
		tapdisk_server_unregister_event(valve->sched_id);
		valve->sched_id = -1;
	}
}

static int
valve_sock_open(td_valve_t *valve)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int s, id, err;

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		PERROR("socket");
		err = -errno;
		goto fail;
	}

	valve->sock = s;

	if (valve->brname[0] == '/')
		strncpy(addr.sun_path, valve->brname,
			sizeof(addr.sun_path));
	else
		snprintf(addr.sun_path, sizeof(addr.sun_path),
			 "%s/%s", TD_VALVE_SOCKDIR, valve->brname);

	err = connect(valve->sock, &addr, sizeof(addr));
	if (err) {
		err = -errno;
		goto fail;
	}

	id = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					   valve->sock, 0,
					   __valve_sock_event,
					   valve);
	if (id < 0) {
		err = id;
		goto fail;
	}

	valve->sock_id = id;

	id = tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT,
					   -1, 0,
					   __valve_sched_event,
					   valve);
	if (id < 0) {
		err = id;
		goto fail;
	}

	valve->sched_id = id;

	INFO("Connected to %s", addr.sun_path);

	valve->cred = 0;
	valve->need = 0;
	valve->done = 0;

	valve_clear_done_pending(valve);

	return 0;

fail:
	valve_sock_close(valve);
	return err;
}

static int
valve_sock_send(td_valve_t *valve, const void *msg, size_t size)
{
	ssize_t n;

	n = send(valve->sock, msg, size, MSG_DONTWAIT);
	if (n < 0)
		return -errno;
	if (n != size)
		return -EPROTO;

	return 0;
}

static int
valve_sock_recv(td_valve_t *valve, void *msg, size_t size)
{
	ssize_t n;

	n = recv(valve->sock, msg, size, MSG_DONTWAIT);
	if (n < 0)
		return -errno;

	return n;
}

static void
__valve_retry_timeout(event_id_t id, char mode, void *private)
{
	td_valve_t *valve = private;
	int err;

	err = valve_sock_open(valve);
	if (!err)
		tapdisk_server_unregister_event(valve->retry_id);
}

static void
valve_schedule_retry(td_valve_t *valve)
{
	int id;

	BUG_ON(valve->sock_id >= 0);

	id = tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT,
					   -1, TD_VALVE_CONNECT_INTERVAL,
					   __valve_retry_timeout,
					   valve);
	BUG_ON(id < 0);

	valve->retry_id = id;
}

static void
valve_conn_open(td_valve_t *valve)
{
	int err;

	BUG_ON(valve->flags & TD_VALVE_KILLED);

	err = valve_sock_open(valve);
	if (err) {
		WARN("%s: %s", valve->brname, strerror(-err));
		valve_schedule_retry(valve);
	}
}

static void
valve_conn_close(td_valve_t *valve, int reset)
{
	td_valve_request_t *req, *next;

	valve_sock_close(valve);

	if (reset)
		td_valve_for_each_stored_request(req, next, valve) {
			td_forward_request(req->treq);
			valve->stats.forw++;
			valve_free_request(valve, req);
		}

	WARN_ON(!list_empty(&valve->stor));
}

static void
valve_conn_reset(td_valve_t *valve)
{
	valve_conn_close(valve, 1);
	valve_conn_open(valve);
}

void
valve_conn_receive(td_valve_t *valve)
{
	unsigned long buf[32], cred = 0;
	ssize_t n;
	int i, err;

	n = valve_sock_recv(valve, buf, sizeof(buf));
	if (!n) {
		err = -ECONNRESET;
		goto reset;
	}

	if (n < 0) {
		err = n;
		if (err != -EAGAIN)
			goto reset;
	}

	for (i = 0; i < n / sizeof(buf[0]); i++) {
		err = WARN_ON(buf[i] >= TD_RLB_REQUEST_MAX);
		if (err)
			goto kill;

		cred += buf[i];
	}

	if (cred > valve->need) {
		err = -EINVAL;
		goto reset;
	}

	valve->cred += cred;
	valve->need -= cred;

	return;

reset:
	VERR(err, "resetting connection");
	valve_conn_reset(valve);
	return;

kill:
	ERR("Killing valve.");
	valve_kill(valve);
}

static void
valve_conn_request(td_valve_t *valve, unsigned long size)
{
	struct td_valve_req _req;
	int err;

	_req.need    = size;
	_req.done    = valve->done;

	valve->need += size;
	valve->done  = 0;

	valve_clear_done_pending(valve);

	err = valve_sock_send(valve, &_req, sizeof(_req));
	if (!err)
		return;

	VERR(err, "resetting connection");
	valve_conn_reset(valve);
}

static int
valve_expend_request(td_valve_t *valve, const td_request_t treq)
{
	if (valve->flags & TD_VALVE_KILLED)
		return 0;

	if (valve->sock < 0)
		return 0;

	if (valve->cred < TREQ_SIZE(treq))
		return -EAGAIN;

	valve->cred -= TREQ_SIZE(treq);

	return 0;
}

static void
__valve_complete_treq(td_request_t treq, int error)
{
	td_valve_request_t *req = treq.cb_data;
	td_valve_t *valve = req->valve;

	BUG_ON(req->secs < treq.secs);
	req->secs -= treq.secs;

	valve->done += TREQ_SIZE(treq);
	valve_set_done_pending(valve);

	if (!req->secs) {
		td_complete_request(req->treq, error);
		valve_free_request(valve, req);
	}
}

static void
valve_forward_stored_requests(td_valve_t *valve)
{
	td_valve_request_t *req, *next;
	td_request_t clone;
	int err;

	td_valve_for_each_stored_request(req, next, valve) {

		err = valve_expend_request(valve, req->treq);
		if (err)
			break;

		clone         = req->treq;
		clone.cb      = __valve_complete_treq;
		clone.cb_data = req;

		td_forward_request(clone);
		valve->stats.forw++;

		list_move(&req->entry, &valve->forw);
	}
}

static int
valve_store_request(td_valve_t *valve, td_request_t treq)
{
	td_valve_request_t *req;

	req = valve_alloc_request(valve);
	if (!req)
		return -EBUSY;

	valve_conn_request(valve, TREQ_SIZE(treq));

	req->treq = treq;
	req->secs = treq.secs;

	list_add_tail(&req->entry, &valve->stor);
	valve->stats.stor++;

	return 0;
}

static void
valve_kill(td_valve_t *valve)
{
	valve->flags |= TD_VALVE_KILLED;
	valve_conn_close(valve, 1);
}

static void
valve_init(td_valve_t *valve, unsigned long flags)
{
	int i;

	memset(valve, 0, sizeof(*valve));

	INIT_LIST_HEAD(&valve->stor);
	INIT_LIST_HEAD(&valve->forw);

	valve->sock     = -1;
	valve->sock_id  = -1;

	valve->retry_id = -1;
	valve->sched_id = -1;

	valve->flags    = flags;

	for (i = ARRAY_SIZE(valve->reqv) - 1; i >= 0; i--) {
		td_valve_request_t *req = &valve->reqv[i];

		req->valve = valve;
		INIT_LIST_HEAD(&req->entry);

		valve_free_request(valve, req);
	}
}

static int
td_valve_close(td_driver_t *driver)
{
	td_valve_t *valve = driver->data;

	WARN_ON(!list_empty(&valve->stor));
	WARN_ON(!list_empty(&valve->forw));

	valve_conn_close(valve, 0);

	if (valve->brname) {
		free(valve->brname);
		valve->brname = NULL;
	}

	return 0;
}

static int
td_valve_open(td_driver_t *driver,
	      const char *name, td_flag_t flags)
{
	td_valve_t *valve = driver->data;
	int err;

	valve_init(valve, TD_VALVE_WRLIMIT);

	valve->brname = strdup(name);
	if (!valve->brname) {
		err = -errno;
		goto fail;
	}

	valve_conn_open(valve);

	return 0;

fail:
	td_valve_close(driver);
	return err;
}

static void
td_valve_queue_request(td_driver_t *driver, td_request_t treq)
{
	td_valve_t *valve = driver->data;
	int err;

	switch (treq.op) {

	case TD_OP_READ:
		if (valve->flags & TD_VALVE_RDLIMIT)
			break;

		goto forward;

	case TD_OP_WRITE:
		if (valve->flags & TD_VALVE_WRLIMIT)
			break;

		goto forward;

	default:
		BUG();
	}

	err = valve_expend_request(valve, treq);
	if (!err)
		goto forward;

	err = valve_store_request(valve, treq);
	if (err)
		td_complete_request(treq, -EBUSY);

	return;

forward:
	td_forward_request(treq);
	valve->stats.forw++;
}

static int
td_valve_get_parent_id(td_driver_t *driver, td_disk_id_t *id)
{
	return -EINVAL;
}

static int
td_valve_validate_parent(td_driver_t *driver,
			 td_driver_t *parent_driver, td_flag_t flags)
{
	return -EINVAL;
}

static void
td_valve_stats(td_driver_t *driver, td_stats_t *st)
{
	td_valve_t *valve = driver->data;
	td_valve_request_t *req, *next;
	int n_reqs;

	tapdisk_stats_field(st, "bridge", "d", valve->brname);
	tapdisk_stats_field(st, "flags", "#x", valve->flags);

	tapdisk_stats_field(st, "cred", "d", valve->cred);
	tapdisk_stats_field(st, "need", "d", valve->need);
	tapdisk_stats_field(st, "done", "d", valve->done);

	/*
	 * stored is [ waiting, total-waits ]
	 */

	n_reqs = 0;
	td_valve_for_each_stored_request(req, next, valve)
		n_reqs++;

	tapdisk_stats_field(st, "stor", "[");
	tapdisk_stats_val(st, "d", n_reqs);
	tapdisk_stats_val(st, "llu", valve->stats.stor);
	tapdisk_stats_leave(st, ']');

	/*
	 * forwarded is [ in-flight, total-requests ]
	 */

	n_reqs = 0;
	td_valve_for_each_forwarded_request(req, next, valve)
		n_reqs++;

	tapdisk_stats_field(st, "forw", "[");
	tapdisk_stats_val(st, "d", n_reqs);
	tapdisk_stats_val(st, "llu", valve->stats.forw);
	tapdisk_stats_leave(st, ']');
}

struct tap_disk tapdisk_valve = {
	.disk_type                  = "tapdisk_valve",
	.flags                      = 0,
	.private_data_size          = sizeof(td_valve_t),
	.td_open                    = td_valve_open,
	.td_close                   = td_valve_close,
	.td_queue_read              = td_valve_queue_request,
	.td_queue_write             = td_valve_queue_request,
	.td_get_parent_id           = td_valve_get_parent_id,
	.td_validate_parent         = td_valve_validate_parent,
	.td_stats                   = td_valve_stats,
};
