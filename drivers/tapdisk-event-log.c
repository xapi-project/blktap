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

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "tapdisk-event-log.h"
#include "tapdisk-log.h"

#ifndef MIN
#define MIN(a, b) ((a) <= (b) ? (a) : (b))
#endif

void
td_event_log_init(struct event_log *log)
{
	memset(log, 0, sizeof(struct event_log));
}

static struct event_log_entry*
td_event_log_add(struct event_log *log)
{
	struct event_log_entry *entry;

	entry = __td_event_log_entry(log->count, log);

	gettimeofday(&entry->ts, NULL);
	entry->fds = 0;

	log->count++;

	return entry;
}


void
td_event_log_add_events(struct event_log *log, int nfds,
			const fd_set *rdfds, const fd_set *wrfds,
			const fd_set *exfds)
{
	struct event_log_entry *entry;
	int i;

	if (nfds < 0)
		return;

	entry = td_event_log_add(log);

	if (!nfds)
		return;

	for (i = 0; i < sizeof(entry->fds) * 8; ++i) {
		int count =
			(rdfds ? FD_ISSET(i, rdfds) : 0) +
			(wrfds ? FD_ISSET(i, wrfds) : 0) +
			(exfds ? FD_ISSET(i, exfds) : 0);
		if (!count)
			continue;

		entry->fds |= (1<<i);

		nfds -= count;
		if (!nfds)
			break;
	}

	if (nfds)
		entry->fds |= TD_EVENT_LOG_OTHER;
}

void
td_event_log_write(struct event_log *log, int level)
{
	int i, j, N;
	struct event_log_entry *prev;

	N = td_event_log_num_entries(log);

	prev = NULL;

	for (i = 0; i < N; i += j) {
		char buf[256], *pos = buf;

		for (j = 0; j < 8 && i+j < N; ++j) {
			struct event_log_entry *entry;
			struct timeval delta;

			delta.tv_sec = delta.tv_usec = 0;

			entry = td_event_log_entry(i+j, log);

			if (prev)
				timersub(&entry->ts, &prev->ts, &delta);

			pos += snprintf(pos, buf + sizeof(buf) - pos,
					" %ld.%06ld:%lx",
					delta.tv_sec, delta.tv_usec,
					entry->fds);

			prev = entry;
		}

		tlog_write(level, "[%lld/%lld]:%s\n",
			   log->count - N + i, log->count, buf);
	}
}
