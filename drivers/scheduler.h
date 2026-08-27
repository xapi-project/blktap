/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include <sys/select.h>
#include <stdint.h>

#include "list.h"

#define SCHEDULER_POLL_READ_FD       0x1
#define SCHEDULER_POLL_WRITE_FD      0x2
#define SCHEDULER_POLL_EXCEPT_FD     0x4
#define SCHEDULER_POLL_TIMEOUT       0x8

typedef int32_t                      event_id_t;
typedef void (*event_cb_t)          (event_id_t id, char mode, void *private);

typedef struct scheduler {
	fd_set                       read_fds;
	fd_set                       write_fds;
	fd_set                       except_fds;

	struct list_head             events;

	event_id_t                   uuid;
	int                          uuid_overflow;
	int                          max_fd;
	struct timeval               timeout;
	struct timeval               max_timeout;
	int                          depth;
} scheduler_t;


void scheduler_initialize(scheduler_t *);

/**
 * Registers an event.
 *
 * Returns the event ID (a positive integer) or a negative error code.
 */
event_id_t scheduler_register_event(scheduler_t *, char mode,
				    int fd, struct timeval timeout,
				    event_cb_t cb, void *private);

event_id_t scheduler_get_event_uuid(scheduler_t *);
void scheduler_unregister_event(scheduler_t *,  event_id_t);
void scheduler_mask_event(scheduler_t *, event_id_t, int masked);
void scheduler_set_max_timeout(scheduler_t *, struct timeval);
int scheduler_wait_for_events(scheduler_t *);
int scheduler_event_set_timeout(scheduler_t *sched, event_id_t event_id,
		struct timeval timeo);
#endif
