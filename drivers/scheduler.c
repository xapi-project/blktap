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
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include "tapdisk.h"
#include "scheduler.h"
#include "tapdisk-log.h"

#define DBG(_f, _a...)               if (0) { tlog_syslog(TLOG_DBG, _f, ##_a); }
#define BUG_ON(_cond)                if (_cond) td_panic()

#define SCHEDULER_MAX_TIMEOUT        600
#define SCHEDULER_POLL_FD           (SCHEDULER_POLL_READ_FD |	\
				     SCHEDULER_POLL_WRITE_FD |	\
				     SCHEDULER_POLL_EXCEPT_FD)

#define MIN(a, b)                   ((a) <= (b) ? (a) : (b))
#define MAX(a, b)                   ((a) >= (b) ? (a) : (b))

#define scheduler_for_each_event(s, event)	\
	list_for_each_entry(event, &(s)->events, next)
#define scheduler_for_each_event_safe(s, event, tmp)	\
	list_for_each_entry_safe(event, tmp, &(s)->events, next)

typedef struct event {
	char                         mode;
	char                         dead;
	char                         pending;
	char                         masked;

	event_id_t                   id;

	int                          fd;
	int                          timeout;
	int                          deadline;

	event_cb_t                   cb;
	void                        *private;

	struct list_head             next;
} event_t;

static void
scheduler_prepare_events(scheduler_t *s)
{
	int diff;
	struct timeval now;
	event_t *event;

	FD_ZERO(&s->read_fds);
	FD_ZERO(&s->write_fds);
	FD_ZERO(&s->except_fds);

	s->max_fd  = -1;
	s->timeout = SCHEDULER_MAX_TIMEOUT;

	gettimeofday(&now, NULL);

	scheduler_for_each_event(s, event) {
		if (event->masked || event->dead)
			continue;

		if (event->mode & SCHEDULER_POLL_READ_FD) {
			FD_SET(event->fd, &s->read_fds);
			s->max_fd = MAX(event->fd, s->max_fd);
		}

		if (event->mode & SCHEDULER_POLL_WRITE_FD) {
			FD_SET(event->fd, &s->write_fds);
			s->max_fd = MAX(event->fd, s->max_fd);
		}

		if (event->mode & SCHEDULER_POLL_EXCEPT_FD) {
			FD_SET(event->fd, &s->except_fds);
			s->max_fd = MAX(event->fd, s->max_fd);
		}

		if (event->mode & SCHEDULER_POLL_TIMEOUT) {
			diff = event->deadline - now.tv_sec;
			if (diff > 0)
				s->timeout = MIN(s->timeout, diff);
			else
				s->timeout = 0;
		}
	}

	s->timeout = MIN(s->timeout, s->max_timeout);
}

static int
scheduler_check_fd_events(scheduler_t *s, int nfds)
{
	event_t *event;

	scheduler_for_each_event(s, event) {
		if (!nfds)
			break;

		if (event->dead)
			continue;

		if ((event->mode & SCHEDULER_POLL_READ_FD) &&
		    FD_ISSET(event->fd, &s->read_fds)) {
			FD_CLR(event->fd, &s->read_fds);
			event->pending |= SCHEDULER_POLL_READ_FD;
			--nfds;
		}

		if ((event->mode & SCHEDULER_POLL_WRITE_FD) &&
		    FD_ISSET(event->fd, &s->write_fds)) {
			FD_CLR(event->fd, &s->write_fds);
			event->pending |= SCHEDULER_POLL_WRITE_FD;
			--nfds;
		}

		if ((event->mode & SCHEDULER_POLL_EXCEPT_FD) &&
		    FD_ISSET(event->fd, &s->except_fds)) {
			FD_CLR(event->fd, &s->except_fds);
			event->pending |= SCHEDULER_POLL_EXCEPT_FD;
			--nfds;
		}
	}

	return nfds;
}

static void
scheduler_check_timeouts(scheduler_t *s)
{
	struct timeval now;
	event_t *event;

	gettimeofday(&now, NULL);

	scheduler_for_each_event(s, event) {
		BUG_ON(event->pending && event->masked);

		if (event->dead)
			continue;

		if (event->pending)
			continue;

		if (!(event->mode & SCHEDULER_POLL_TIMEOUT))
			continue;

		if (event->deadline > now.tv_sec)
			continue;

		event->pending = SCHEDULER_POLL_TIMEOUT;
	}
}

static int
scheduler_check_events(scheduler_t *s, int nfds)
{
	if (nfds)
		nfds = scheduler_check_fd_events(s, nfds);

	scheduler_check_timeouts(s);

	return nfds;
}

static void
scheduler_event_callback(event_t *event, char mode)
{
	if (event->mode & SCHEDULER_POLL_TIMEOUT) {
		struct timeval now;
		gettimeofday(&now, NULL);
		event->deadline = now.tv_sec + event->timeout;
	}

	if (!event->masked)
		event->cb(event->id, mode, event->private);
}

static int
scheduler_run_events(scheduler_t *s)
{
	event_t *event;
	int n_dispatched = 0;

	scheduler_for_each_event(s, event) {
		char pending;

		if (event->dead)
			continue;

		pending = event->pending;
		if (pending) {
			event->pending = 0;
			/* NB. must clear before cb */
			scheduler_event_callback(event, pending);
			n_dispatched++;
		}
	}

	return n_dispatched;
}

int
scheduler_register_event(scheduler_t *s, char mode, int fd,
			 int timeout, event_cb_t cb, void *private)
{
	event_t *event;
	struct timeval now;

	if (!cb)
		return -EINVAL;

	if (!(mode & SCHEDULER_POLL_TIMEOUT) && !(mode & SCHEDULER_POLL_FD))
		return -EINVAL;

	event = calloc(1, sizeof(event_t));
	if (!event)
		return -ENOMEM;

	gettimeofday(&now, NULL);

	INIT_LIST_HEAD(&event->next);

	event->mode     = mode;
	event->fd       = fd;
	event->timeout  = timeout;
	event->deadline = now.tv_sec + timeout;
	event->cb       = cb;
	event->private  = private;
	event->id       = s->uuid++;
	event->masked   = 0;

	if (!s->uuid)
		s->uuid++;

	list_add_tail(&event->next, &s->events);

	return event->id;
}

void
scheduler_unregister_event(scheduler_t *s, event_id_t id)
{
	event_t *event, *tmp;

	if (!id)
		return;

	scheduler_for_each_event(s, event)
		if (event->id == id) {
			event->dead = 1;
			break;
		}
}

void
scheduler_mask_event(scheduler_t *s, event_id_t id, int masked)
{
	event_t *event;

	if (!id)
		return;

	scheduler_for_each_event(s, event)
		if (event->id == id) {
			event->masked = !!masked;
			break;
		}
}

static void
scheduler_gc_events(scheduler_t *s)
{
	event_t *event, *next;

	scheduler_for_each_event_safe(s, event, next)
		if (event->dead) {
			list_del(&event->next);
			free(event);
		}
}

void
scheduler_set_max_timeout(scheduler_t *s, int timeout)
{
	if (timeout >= 0)
		s->max_timeout = MIN(s->max_timeout, timeout);
}

int
scheduler_wait_for_events(scheduler_t *s)
{
	int ret;
	struct timeval tv;

	s->depth++;
	ret = 0;

	if (s->depth > 1 && scheduler_run_events(s))
		/* NB. recursive invocations continue with the pending
		 * event set. We return as soon as we made some
		 * progress. */
		goto out;

	scheduler_prepare_events(s);

	tv.tv_sec  = s->timeout;
	tv.tv_usec = 0;

	DBG("timeout: %d, max_timeout: %d\n",
	    s->timeout, s->max_timeout);

	ret = select(s->max_fd + 1, &s->read_fds,
		     &s->write_fds, &s->except_fds, &tv);

	if (ret < 0)
		goto out;

	ret = scheduler_check_events(s, ret);
	BUG_ON(ret);

	s->timeout     = SCHEDULER_MAX_TIMEOUT;
	s->max_timeout = SCHEDULER_MAX_TIMEOUT;

	scheduler_run_events(s);

	if (s->depth == 1)
		scheduler_gc_events(s);

out:
	s->depth--;

	return ret;
}

void
scheduler_initialize(scheduler_t *s)
{
	memset(s, 0, sizeof(scheduler_t));

	s->uuid  = 1;
	s->depth = 0;

	FD_ZERO(&s->read_fds);
	FD_ZERO(&s->write_fds);
	FD_ZERO(&s->except_fds);

	INIT_LIST_HEAD(&s->events);
}
