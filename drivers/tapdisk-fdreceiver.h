/* 
 * Unix domain socket fd receiver
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

typedef void (*fd_cb_t) (int fd, char *msg, void *data);

struct td_fdreceiver *td_fdreceiver_start(char *path, fd_cb_t, void *data);
void td_fdreceiver_stop(struct td_fdreceiver *);

struct td_fdreceiver {
	char *path;

	int fd;
	int fd_event_id;

	int client_fd;
	int client_event_id;

	fd_cb_t callback;
	void *callback_data;
};
