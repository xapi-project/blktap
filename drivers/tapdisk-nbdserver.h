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

#include "blktap.h"
#include "tapdisk-vbd.h"
#include "list.h"

struct td_nbdserver {
	td_vbd_t               *vbd;
	td_disk_info_t          info;

	int                     listening_fd;
	int                     listening_event_id;

	struct td_fdreceiver   *fdreceiver;
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
};

td_nbdserver_t *tapdisk_nbdserver_alloc(td_vbd_t *, td_disk_info_t);
int tapdisk_nbdserver_listen(td_nbdserver_t *, int);
void tapdisk_nbdserver_free(td_nbdserver_t *);
void tapdisk_nbdserver_pause(td_nbdserver_t *);
int tapdisk_nbdserver_unpause(td_nbdserver_t *);

#endif /* _TAPDISK_NBDSERVER_H_ */
