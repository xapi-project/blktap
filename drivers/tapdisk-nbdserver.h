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

#ifndef _TAPDISK_NBDSERVER_H_
#define _TAPDISK_NBDSERVER_H_

typedef struct td_nbdserver td_nbdserver_t;
typedef struct td_nbdserver_req td_nbdserver_req_t;
typedef struct td_nbdserver_client td_nbdserver_client_t;

#include "blktap2.h"
#include "tapdisk-vbd.h"
#include "list.h"
#include "tapdisk-nbd.h"
#include <sys/un.h>
#include <stdbool.h>

struct td_nbdserver {
	td_vbd_t               *vbd;
	td_disk_info_t          info;

	/**
	 * Listening file descriptor for the file descriptor receiver.
	 */
	int                     fdrecv_listening_fd;

	/**
	 * Event ID for the file descriptor receiver.
	 */
	int                     fdrecv_listening_event_id;

	struct td_fdreceiver   *fdreceiver;

	/**
	 * Listening file descriptor for the NBD server on the UNIX domain socket.
	 */
	int                     unix_listening_fd;

	/**
	 * Event ID for the file descriptor receiver.
	 */
	int                     unix_listening_event_id;

	/**
	 * Socket for the UNIX domain socket.
	 */
	struct sockaddr_un		local;

	/**
	 * UNIX domain socket path.
	 */
	char                    sockpath[TAPDISK_NBDSERVER_MAX_PATH_LEN];

	struct list_head        clients;
};

struct td_nbdserver_client {
	int                     n_reqs;
	td_nbdserver_req_t     *reqs;
	struct td_iovec        *iovecs;
	int                     n_reqs_free;
	td_nbdserver_req_t    **reqs_free;

	int                     client_fd;
	int                     client_event_id;

	td_nbdserver_t         *server;
	struct list_head        clientlist;

	int                     paused;

	bool                    dead;
};

td_nbdserver_t *tapdisk_nbdserver_alloc(td_vbd_t *, td_disk_info_t);

/**
 * Listen for connections on a TCP socket at the specified port.
 */
int tapdisk_nbdserver_listen_inet(td_nbdserver_t *server, const int port);

/**
 * Listen for connections on a UNIX domain socket.
 */
int tapdisk_nbdserver_listen_unix(td_nbdserver_t *server);

void tapdisk_nbdserver_free(td_nbdserver_t *);
void tapdisk_nbdserver_pause(td_nbdserver_t *);
int tapdisk_nbdserver_unpause(td_nbdserver_t *);

/**
 * Tells how many requests are pending.
 */
int tapdisk_nbdserver_reqs_pending(td_nbdserver_client_t *client);

#endif /* _TAPDISK_NBDSERVER_H_ */
