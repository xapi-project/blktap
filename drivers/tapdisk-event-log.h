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

#ifndef _EVENT_LOG_H_
#define _EVENT_LOG_H_

#include <sys/select.h>

#define TD_EVENT_LOG_N_ENTRIES 128

struct event_log_entry {
	struct timeval          ts;
	long                    fds;
};

struct event_log {
	struct event_log_entry  ring[TD_EVENT_LOG_N_ENTRIES];
	unsigned long long      count;
};

/* NB. Reuse low fds <= 2, these are dupd to /dev/null */
#define TD_EVENT_LOG_OTHER      (1<<0)

#define td_event_log_num_entries(_s)			\
	MIN((_s)->count, TD_EVENT_LOG_N_ENTRIES)

#define td_event_log_first(_s)				\
	((_s)->count < TD_EVENT_LOG_N_ENTRIES		\
	 ? 0						\
	 : (_s)->count % TD_EVENT_LOG_N_ENTRIES)

#define __td_event_log_entry(_i, _s)			\
	&(_s)->ring[(_i) % TD_EVENT_LOG_N_ENTRIES]

#define td_event_log_entry(_n, _s)			\
	__td_event_log_entry(td_event_log_first(_s) + (_n), _s)

void td_event_log_init(struct event_log *log);

void td_event_log_add_events(struct event_log *log, int nfds,
			     const fd_set *rdfds, const fd_set *wrfds,
			     const fd_set *exfds);

void td_event_log_write(struct event_log *log, int level);

#endif /* _EVENT_LOG_H_ */
