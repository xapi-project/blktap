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
#ifndef _TAPDISK_DISPATCH_H_
#define _TAPDISK_DISPATCH_H_

#include "xs_api.h"
#include "blktaplib.h"
#include "tapdisk-message.h"

typedef enum {
	TAPDISK_CHANNEL_DEAD          = 1,
	TAPDISK_CHANNEL_LAUNCHED      = 2,
	TAPDISK_CHANNEL_WAIT_PID      = 3,
	TAPDISK_CHANNEL_PID           = 4,
	TAPDISK_CHANNEL_WAIT_OPEN     = 5,
	TAPDISK_CHANNEL_RUNNING       = 6,
	TAPDISK_CHANNEL_WAIT_PAUSE    = 7,
	TAPDISK_CHANNEL_PAUSED        = 8,
	TAPDISK_CHANNEL_WAIT_RESUME   = 9,
	TAPDISK_CHANNEL_WAIT_CLOSE    = 10,
	TAPDISK_CHANNEL_CLOSED        = 11,
} channel_state_t;

#define TAPDISK_CHANNEL_IPC_OPEN(_c)		    \
	((_c)->state != TAPDISK_CHANNEL_DEAD     && \
	 (_c)->state != TAPDISK_CHANNEL_CLOSED)

#define TAPDISK_CHANNEL_IPC_IDLE(_c)		   \
	((_c)->state == TAPDISK_CHANNEL_LAUNCHED || \
	 (_c)->state == TAPDISK_CHANNEL_PID      || \
	 (_c)->state == TAPDISK_CHANNEL_RUNNING  || \
	 (_c)->state == TAPDISK_CHANNEL_PAUSED)

typedef enum {
	TAPDISK_VBD_UNPAUSED        = 1,
	TAPDISK_VBD_PAUSING         = 2,
	TAPDISK_VBD_PAUSED          = 3,
	TAPDISK_VBD_BROKEN          = 4,
	TAPDISK_VBD_DEAD            = 5,
	TAPDISK_VBD_RECYCLED        = 6,
} vbd_state_t;

typedef enum {
	TAPDISK_VBD_UP              = 1,
	TAPDISK_VBD_DOWN            = 2,
} shutdown_state_t;

struct tapdisk_channel {
	channel_state_t           state;
	vbd_state_t               vbd_state;
	shutdown_state_t          shutdown_state;
	int                       shutdown_force;

	int                       read_fd;
	int                       write_fd;
	int                       blktap_fd;
	int                       channel_id;

	char                      mode;
	char                      shared;
	unsigned int              domid;
	unsigned int              busid;
	unsigned int              major;
	unsigned int              minor;
	unsigned int              storage;
	unsigned int              drivertype;
	uint16_t                  cookie;
	pid_t                     tapdisk_pid;

	char                     *path;
	char                     *frontpath;
	char                     *params;
	char                     *vdi_path;
	char                     *uuid_str;
	char                     *start_str;
	char                     *pause_str;
	char                     *pause_done_str;
	char                     *share_tapdisk_str;

	image_t                   image;

	struct list_head          list;
	struct xenbus_watch       start_watch;
	struct xenbus_watch       pause_watch;

	struct xs_handle         *xsh;
};

typedef struct tapdisk_channel tapdisk_channel_t;

int strsep_len(const char *str, char c, unsigned int len);
int make_blktap_device(char *devname, int major, int minor, int perm);

int tapdisk_channel_open(tapdisk_channel_t **,
			 const char *node, struct xs_handle *,
			 int blktap_fd, uint16_t cookie,
			 int domid, int busid);

void tapdisk_daemon_maybe_clone_channel(tapdisk_channel_t *);
void tapdisk_daemon_close_channel(tapdisk_channel_t *);

int tapdisk_channel_receive_message(tapdisk_channel_t *, tapdisk_message_t *);
void tapdisk_channel_reap(tapdisk_channel_t *, int status);
int tapdisk_channel_change_vbd_state(tapdisk_channel_t *, vbd_state_t);
void tapdisk_channel_drive_vbd_state(tapdisk_channel_t *);
int tapdisk_channel_check_uuid(tapdisk_channel_t *);

#endif
