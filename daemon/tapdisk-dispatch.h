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

struct tapdisk_channel {
	int                       state;

	int                       read_fd;
	int                       write_fd;
	int                       blktap_fd;
	int                       channel_id;

	char                      mode;
	char                      shared;
	char                      open;
	unsigned int              domid;
	unsigned int              busid;
	unsigned int              major;
	unsigned int              minor;
	unsigned int              storage;
	unsigned int              drivertype;
	uint16_t                  cookie;
	pid_t                     tapdisk_pid;

	/*
	 * special accounting needed to handle pause
	 * requests received before tapdisk process is ready
	 */
	char                      connected;
	char                      pause_needed;

	char                     *path;
	char                     *frontpath;
	char                     *params;
	char                     *vdi_path;
	char                     *uuid_str;
	char                     *pause_str;
	char                     *pause_done_str;
	char                     *shutdown_str;
	char                     *share_tapdisk_str;

	image_t                   image;

	struct list_head          list;
	struct xenbus_watch       pause_watch;
	struct xenbus_watch       shutdown_watch;
	char                      pause_watch_registered;

	struct xs_handle         *xsh;
};

typedef struct tapdisk_channel tapdisk_channel_t;

int strsep_len(const char *str, char c, unsigned int len);
int make_blktap_device(char *devname, int major, int minor, int perm);

int tapdisk_channel_open(tapdisk_channel_t **,
			 char *node, struct xs_handle *,
			 int blktap_fd, uint16_t cookie);
void tapdisk_channel_close(tapdisk_channel_t *);

void tapdisk_daemon_find_channel(tapdisk_channel_t *);
void tapdisk_daemon_close_channel(tapdisk_channel_t *);

int tapdisk_channel_receive_message(tapdisk_channel_t *, tapdisk_message_t *);

#endif
