/*
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
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
};

td_nbdserver_t *tapdisk_nbdserver_alloc(td_vbd_t *, td_disk_info_t);
int tapdisk_nbdserver_listen(td_nbdserver_t *, int);
void tapdisk_nbdserver_free(td_nbdserver_t *);

#endif /* _TAPDISK_NBDSERVER_H_ */
