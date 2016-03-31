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

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>

#include "debug.h"
#include "tapdisk.h"
#include "scheduler.h"
#include "tapdisk-log.h"
#include "timeout-math.h"

#define DBG(_f, _a...)               if (0) { tlog_syslog(TLOG_DBG, _f, ##_a); }
#define BUG_ON(_cond)                if (_cond) td_panic()

#define SCHEDULER_MAX_TIMEOUT        600
#define SCHEDULER_POLL_FD           (SCHEDULER_POLL_READ_FD |	\
				     SCHEDULER_POLL_WRITE_FD |	\
				     SCHEDULER_POLL_EXCEPT_FD)

#define MIN(a, b)                   ((a) <= (b) ? (a) : (b))
#define MAX(a, b)                   ((a) >= (b) ? (a) : (b))

/**
 * Async-signal safe.
 */
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

	/**
	 * Timeout relative to the time of the registration
	 * of the event. Use the special value {(time_t)-1, 0} to indicate infinity.
	 */
	struct timeval               timeout;

	/**
	 * Expiration date in seconds after Epoch. Once current time
	 * becomes larger than or equal to this value, the event is considered
	 * expired and can be run. If event.timeout is set to infinity, this member
	 * should not be used.
	 *
	 */
	struct timeval               deadline;

	event_cb_t                   cb;
	void                        *private;

	struct list_head             next;
} event_t;

static void
scheduler_prepare_events(scheduler_t *s)
{
	struct timeval diff;
	struct timeval now;
	event_t *event;

	FD_ZERO(&s->read_fds);
	FD_ZERO(&s->write_fds);
	FD_ZERO(&s->except_fds);

	s->max_fd  = -1;
	s->timeout = TV_SECS(SCHEDULER_MAX_TIMEOUT);

	gettimeofday(&now, NULL);

	scheduler_for_each_event(s, event) {
		if (event->masked || event->dead)
			continue;

		if (event->mode & SCHEDULER_POLL_FD &&
			(fcntl(event->fd, F_GETFL) < 0) &&
			errno == EBADF)
		{
			EPRINTF("EBADF: Marking event dead, id: %d", event->id);
			event->dead = 1;
			continue;
		}

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

		if (event->mode & SCHEDULER_POLL_TIMEOUT
				&& !TV_IS_INF(event->timeout)) {
			TV_SUB(event->deadline, now, diff);
			if (TV_AFTER(diff, TV_ZERO))
				s->timeout = TV_MIN(s->timeout, diff);
			else
				s->timeout = TV_ZERO;
		}
	}

	s->timeout = TV_MIN(s->timeout, s->max_timeout);
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

/**
 * Checks all scheduler events whose mode is set to SCHEDULER_POLL_TIMEOUT
 * whether their time out has elapsed, and if so it makes them runnable.
 */
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

		if (TV_IS_INF(event->timeout))
			continue;

		if (TV_BEFORE(now, event->deadline))
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
	if (event->mode & SCHEDULER_POLL_TIMEOUT
			&& !TV_IS_INF(event->timeout)) {
		struct timeval now;
		gettimeofday(&now, NULL);
		TV_ADD(now, event->timeout, event->deadline);
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
			 struct timeval timeout, event_cb_t cb, void *private)
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
	if (TV_IS_INF(event->timeout))
		/* initialise it to something meaningful */
		event->deadline = TV_INF;
	else
		TV_ADD(now, timeout, event->deadline);
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
	event_t *event;

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
scheduler_set_max_timeout(scheduler_t *s, struct timeval timeout)
{
	if (!TV_IS_INF(timeout))
		s->max_timeout = TV_MIN(s->max_timeout, timeout);
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

	tv = s->timeout;

	DBG("timeout: %ld.%ld, max_timeout: %ld.%ld\n",
	    s->timeout.tv_sec, s->timeout.tv_usec, s->max_timeout.tv_sec, s->max_timeout.tv_usec);

    do {
    	ret = select(s->max_fd + 1, &s->read_fds, &s->write_fds,
                &s->except_fds, &tv);
        if (ret < 0) {
            ret = -errno;
            ASSERT(ret);
        }
    } while (ret == -EINTR);

    if (ret < 0) {
        EPRINTF("select failed: %s\n", strerror(-ret));
        goto out;
    }

	ret = scheduler_check_events(s, ret);
	BUG_ON(ret);

	s->timeout     = TV_SECS(SCHEDULER_MAX_TIMEOUT);
	s->max_timeout = TV_SECS(SCHEDULER_MAX_TIMEOUT);

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

int
scheduler_event_set_timeout(scheduler_t *sched, event_id_t event_id, struct timeval timeo)
{
	event_t *event;

	ASSERT(sched);

	if (!event_id)
		return -EINVAL;

	scheduler_for_each_event(sched, event) {
		if (event->id == event_id) {
			if (!(event->mode & SCHEDULER_POLL_TIMEOUT))
				return -EINVAL;
			event->timeout = timeo;
			if (TV_IS_INF(event->timeout))
				event->deadline = TV_INF;
			else {
				struct timeval now;
				gettimeofday(&now, NULL);
				TV_ADD(now, event->timeout, event->deadline);
			}
			return 0;
		}
	}

	return -ENOENT;
}

