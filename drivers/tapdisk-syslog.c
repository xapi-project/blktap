/*
 * Copyright (c) 2009, XenSource Inc.
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

/*
 * A non-blocking, buffered BSD syslog client.
 *
 * http://www.ietf.org/rfc/rfc3164.txt (FIXME: Read this.)
 */

#define _ISOC99_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "tapdisk-server.h"
#include "tapdisk-syslog.h"
#include "tapdisk-utils.h"

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

static int tapdisk_syslog_sock_send(td_syslog_t *log,
				    const void *msg, size_t size);
static int tapdisk_syslog_sock_connect(td_syslog_t *log);

static void tapdisk_syslog_sock_mask(td_syslog_t *log);
static void tapdisk_syslog_sock_unmask(td_syslog_t *log);

static const struct sockaddr_un syslog_addr = {
	.sun_family = AF_UNIX,
	.sun_path   = "/dev/log"
};

#define RING_PTR(_log, _idx)                                            \
	(&(_log)->ring[(_idx) % (_log)->ringsz])

#define RING_FREE(_log)                                                 \
	((_log)->ringsz - ((_log)->prod - (_log)->cons))

/*
 * NB. Ring buffer.
 *
 * We allocate a number of pages as indicated by @bufsz during
 * initialization. From that, 1K is reserved for message staging, the
 * rest is cyclic ring space.
 *
 * All producer/consumer offsets wrap on size_t range, not buffer
 * size. Hence the RING() macros.
 */

static void
__tapdisk_syslog_ring_init(td_syslog_t *log)
{
	log->buf     = NULL;
	log->bufsz   = 0;
	log->msg     = NULL;
	log->ring    = NULL;
	log->ringsz  = 0;
}

static inline size_t
page_align(size_t size)
{
	size_t page_size = sysconf(_SC_PAGE_SIZE);
	return (size + page_size - 1) & ~(page_size - 1);
}

static void
tapdisk_syslog_ring_uninit(td_syslog_t *log)
{
	if (log->buf)
		munmap(log->buf, log->bufsz);

	__tapdisk_syslog_ring_init(log);
}

static int
tapdisk_syslog_ring_init(td_syslog_t *log, size_t size)
{
	int prot, flags, err;

	__tapdisk_syslog_ring_init(log);

	log->bufsz = page_align(size);

	prot  = PROT_READ|PROT_WRITE;
	flags = MAP_ANONYMOUS|MAP_PRIVATE;

	log->buf = mmap(NULL, log->bufsz, prot, flags, -1, 0);
	if (log->buf == MAP_FAILED) {
		log->buf = NULL;
		err = -ENOMEM;
		goto fail;
	}

	err = mlock(log->buf, size);
	if (err) {
		err = -errno;
		goto fail;
	}

	log->msg    = log->buf;
	log->ring   = log->buf + TD_SYSLOG_PACKET_MAX;
	log->ringsz = size     - TD_SYSLOG_PACKET_MAX;

	return 0;

fail:
	tapdisk_syslog_ring_uninit(log);

	return err;
}

static int
tapdisk_syslog_ring_write_str(td_syslog_t *log, const char *msg, size_t len)
{
	size_t size, prod, i;

	len  = MIN(len, TD_SYSLOG_PACKET_MAX);
	size = len + 1;

	if (size > RING_FREE(log))
		return -ENOBUFS;

	prod = log->prod;

	for (i = 0; i < len; ++i) {
		char c;

		c = msg[i];
		if (c == 0)
			break;

		*RING_PTR(log, prod) = c;
		prod++;
	}

	*RING_PTR(log, prod) = 0;

	log->prod = prod + 1;

	return 0;
}

static ssize_t
tapdisk_syslog_ring_read_pkt(td_syslog_t *log, char *msg, size_t size)
{
	size_t cons;
	ssize_t sz;

	size = MIN(size, TD_SYSLOG_PACKET_MAX);

	sz   = 0;
	cons = log->cons;

	while (sz < size) {
		char c;

		if (cons == log->prod)
			break;

		c = *RING_PTR(log, cons);
		msg[sz++] = c;
		cons++;

		if (c == 0)
			break;
	}

	return sz - 1;
}

static int
tapdisk_syslog_ring_dispatch_one(td_syslog_t *log)
{
	size_t len;
	int err;

	len = tapdisk_syslog_ring_read_pkt(log, log->msg,
					   TD_SYSLOG_PACKET_MAX);
	if (len == -1)
		return -ENOMSG;

	err = tapdisk_syslog_sock_send(log, log->msg, len);

	if (err == -EAGAIN)
		return err;

	if (err)
		goto fail;

done:
	log->cons += len + 1;
	return 0;

fail:
	log->stats.fails++;
	goto done;
}

static void
tapdisk_syslog_ring_warning(td_syslog_t *log)
{
	int n, err;

	n        = log->oom;
	log->oom = 0;

	err = tapdisk_syslog(log, LOG_WARNING,
			     "tapdisk-syslog: %d messages dropped", n);
	if (err)
		log->oom = n;
}

static void
tapdisk_syslog_ring_dispatch(td_syslog_t *log)
{
	int err;

	do {
		err = tapdisk_syslog_ring_dispatch_one(log);
	} while (!err);

	if (log->oom)
		tapdisk_syslog_ring_warning(log);
}

static int
tapdisk_syslog_vsprintf(char *buf, size_t size,
			int prio, const struct timeval *tv, const char *ident,
			const char *fmt, va_list ap)
{
	char tsbuf[TD_SYSLOG_STRTIME_LEN+1];
	size_t len;

	/*
	 * PKT       := PRI HEADER MSG
	 * PRI       := "<" {"0" .. "9"} ">"
	 * HEADER    := TIMESTAMP HOSTNAME
	 * MSG       := <TAG> <SEP> <CONTENT>
	 * SEP       := ":" | " " | "["
	 */

	tapdisk_syslog_strftime(tsbuf, sizeof(tsbuf), tv);

	len = 0;

	/* NB. meant to work with c99 null buffers */

	len += snprintf(buf ? buf + len : NULL, buf ? size - len : 0,
			"<%d>%s %s: ", prio, tsbuf, ident);

	len += vsnprintf(buf ? buf + len : NULL, buf ? size - len : 0,
			 fmt, ap);

	return MIN(len, size);
}

/*
 * NB. Sockets.
 *
 * Syslog is based on a connectionless (DGRAM) unix transport.
 *
 * While it is reliable, we cannot block on syslogd because -- as with
 * any IPC in tapdisk -- we could deadlock in page I/O writeback.
 * Hence the syslog(3) avoidance on the datapath, which this code
 * facilitates.
 *
 * This type of socket has a single (global) receive buffer on
 * syslogd's end, but no send buffer at all. The does just that:
 * headroom on the sender side.
 *
 * The transport is rather stateless, but we still need to connect()
 * the socket, or select() will find no receive buffer to block
 * on. While we never disconnect, connections are unreliable because
 * syslog may shut down.
 *
 * Reconnection will be attempted with every user message submitted.
 * Any send() or connect() failure other than EAGAIN discards the
 * message. Also, the write event handler will go on to discard any
 * remaining ring contents as well, once the socket is disconnected.
 *
 * In summary, no attempts to mask service blackouts in here.
 */

int
tapdisk_vsyslog(td_syslog_t *log, int prio, const char *fmt, va_list ap)
{
	struct timeval now;
	size_t len;
	int err;

	gettimeofday(&now, NULL);

	len = tapdisk_syslog_vsprintf(log->msg, TD_SYSLOG_PACKET_MAX,
				      prio | log->facility,
				      &now, log->ident, fmt, ap);

	log->stats.count += 1;
	log->stats.bytes += len;

	if (log->cons != log->prod)
		goto busy;

send:
	err = tapdisk_syslog_sock_send(log, log->msg, len);
	if (!err)
		return 0;

	if (err == -ENOTCONN) {
		err = tapdisk_syslog_sock_connect(log);
		if (!err)
			goto send;
	}

	if (err != -EAGAIN)
		goto fail;

	tapdisk_syslog_sock_unmask(log);

busy:
	if (log->oom) {
		err = -ENOBUFS;
		goto oom;
	}

	err = tapdisk_syslog_ring_write_str(log, log->msg, len);
	if (!err)
		return 0;

	log->oom_tv = now;

oom:
	log->oom++;
	log->stats.drops++;
	return err;

fail:
	log->stats.fails++;
	return err;
}

int
tapdisk_syslog(td_syslog_t *log, int prio, const char *fmt, ...)
{
	va_list ap;
	int err;

	va_start(ap, fmt);
	err = tapdisk_vsyslog(log, prio, fmt, ap);
	va_end(ap);

	return err;
}

static int
tapdisk_syslog_sock_send(td_syslog_t *log, const void *msg, size_t size)
{
	ssize_t n;

	log->stats.xmits++;

	n = send(log->sock, msg, size, MSG_DONTWAIT);
	if (n < 0)
		return -errno;

	return 0;
}

static void
tapdisk_syslog_sock_event(event_id_t id, char mode, void *private)
{
	td_syslog_t *log = private;

	tapdisk_syslog_ring_dispatch(log);

	if (log->cons == log->prod)
		tapdisk_syslog_sock_mask(log);
}

static void
__tapdisk_syslog_sock_init(td_syslog_t *log)
{
	log->sock     = -1;
	log->event_id = -1;
}

static void
tapdisk_syslog_sock_close(td_syslog_t *log)
{
	if (log->sock >= 0)
		close(log->sock);

	if (log->event_id >= 0)
		tapdisk_server_unregister_event(log->event_id);

	__tapdisk_syslog_sock_init(log);
}

static int
tapdisk_syslog_sock_open(td_syslog_t *log)
{
	event_id_t id;
	int s, err;

	__tapdisk_syslog_sock_init(log);

	s = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (s < 0) {
		err = -errno;
		goto fail;
	}

	log->sock = s;

#if 0
	err = fcntl(s, F_SETFL, O_NONBLOCK);
	if (err < 0) {
		err = -errno;
		goto fail;
	}
#endif

	id = tapdisk_server_register_event(SCHEDULER_POLL_WRITE_FD,
					   s, 0,
					   tapdisk_syslog_sock_event,
					   log);
	if (id < 0) {
		err = id;
		goto fail;
	}

	log->event_id = id;

	tapdisk_syslog_sock_mask(log);

	return 0;

fail:
	tapdisk_syslog_sock_close(log);
	return err;
}

static int
tapdisk_syslog_sock_connect(td_syslog_t *log)
{
	int err;

	err = connect(log->sock, &syslog_addr, sizeof(syslog_addr));
	if (err < 0)
		err = -errno;

	return err;
}

static void
tapdisk_syslog_sock_mask(td_syslog_t *log)
{
	tapdisk_server_mask_event(log->event_id, 1);
}

static void
tapdisk_syslog_sock_unmask(td_syslog_t *log)
{
	tapdisk_server_mask_event(log->event_id, 0);
}

void
__tapdisk_syslog_init(td_syslog_t *log)
{
	memset(log, 0, sizeof(td_syslog_t));
	__tapdisk_syslog_sock_init(log);
	__tapdisk_syslog_ring_init(log);
}

void
tapdisk_syslog_close(td_syslog_t *log)
{
	tapdisk_syslog_ring_uninit(log);
	tapdisk_syslog_sock_close(log);

	if (log->ident)
		free(log->ident);

	__tapdisk_syslog_init(log);
}

int
tapdisk_syslog_open(td_syslog_t *log, const char *ident, int facility, size_t bufsz)
{
	int err;

	__tapdisk_syslog_init(log);

	log->facility = facility;
	log->ident = ident ? strndup(ident, TD_SYSLOG_IDENT_MAX) : NULL;

	err = tapdisk_syslog_sock_open(log);
	if (err)
		goto fail;

	err = tapdisk_syslog_ring_init(log, bufsz);
	if (err)
		goto fail;

	return 0;

fail:
	tapdisk_syslog_close(log);

	return err;
}

void
tapdisk_syslog_stats(td_syslog_t *log, int prio)
{
	struct _td_syslog_stats *s = &log->stats;

	tapdisk_syslog(log, prio,
		       "tapdisk-syslog: %llu messages, %llu bytes, "
		       "xmits: %llu, failed: %llu, dropped: %llu",
		       s->count, s->bytes,
		       s->xmits, s->fails, s->drops);
}

void
tapdisk_syslog_flush(td_syslog_t *log)
{
	while (log->cons != log->prod)
		tapdisk_server_iterate();
}
