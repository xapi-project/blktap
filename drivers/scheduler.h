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

#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include <sys/select.h>

#include "list.h"

#define SCHEDULER_POLL_READ_FD       0x1
#define SCHEDULER_POLL_WRITE_FD      0x2
#define SCHEDULER_POLL_EXCEPT_FD     0x4
#define SCHEDULER_POLL_TIMEOUT       0x8

typedef int                          event_id_t;
typedef void (*event_cb_t)          (event_id_t id, char mode, void *private);

typedef struct scheduler {
	fd_set                       read_fds;
	fd_set                       write_fds;
	fd_set                       except_fds;

	struct list_head             events;

	int                          uuid;
	int                          max_fd;
	int                          timeout;
	int                          max_timeout;
	int                          depth;
} scheduler_t;

void scheduler_initialize(scheduler_t *);

/**
 * Registers an event.
 *
 * Returns the event ID (a positive integer) or a negative error code.
 */
event_id_t scheduler_register_event(scheduler_t *, char mode,
				    int fd, int timeout,
				    event_cb_t cb, void *private);

void scheduler_unregister_event(scheduler_t *,  event_id_t);
void scheduler_mask_event(scheduler_t *, event_id_t, int masked);
void scheduler_set_max_timeout(scheduler_t *, int);
int scheduler_wait_for_events(scheduler_t *);
int scheduler_event_set_timeout(scheduler_t *sched, event_id_t event_id,
		int timeo);
#endif
