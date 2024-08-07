/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
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

#include "blktap2.h"
#include "tapdisk-vbd.h"
#include "list.h"
#include <sys/un.h>
#include <stdbool.h>

#define NBD_NEGOTIATION_MAGIC 0x00420281861253LL
#define NBD_REQUEST_MAGIC 0x25609513
#define NBD_REPLY_MAGIC 0x67446698

enum {
	TAPDISK_NBD_CMD_READ = 0,
	TAPDISK_NBD_CMD_WRITE,
	TAPDISK_NBD_CMD_DISC,
	TAPDISK_NBD_CMD_FLUSH,
	TAPDISK_NBD_CMD_TRIM,
	TAPDISK_NBD_CMD_CACHE,
	TAPDISK_NBD_CMD_WRITE_ZEROES,
	TAPDISK_NBD_CMD_BLOCK_STATUS
};

struct nbd_request {
	uint32_t magic;
	uint32_t type;	
	char handle[8];
	uint64_t from;
	uint32_t len;
} __attribute__ ((packed));

struct nbd_reply {
	uint32_t magic;
	uint32_t error;		
	char handle[8];		
};


#define TAPDISK_NBDSERVER_MAX_PATH_LEN 256
#define TAPDISK_NBDCLIENT_LISTEN_SOCK_PATH BLKTAP2_CONTROL_DIR"/nbdclient"
#define TAPDISK_NBDSERVER_LISTEN_SOCK_PATH BLKTAP2_CONTROL_DIR"/nbdserver"
#define TAPDISK_NBDSERVER_SOCK_PATH BLKTAP2_CONTROL_DIR"/nbd"

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
	 * Socket opened during handshake negotiation.
	 */
	int                     handshake_fd;

	/**
	 * Event ID for the file descriptor receiver.
	 */
	int                     unix_listening_event_id;

	/**
	 * Socket for the UNIX domain socket.
	 */
	struct sockaddr_un	local;

	/**
	 * UNIX domain socket path.
	 */
	char                    sockpath[TAPDISK_NBDSERVER_MAX_PATH_LEN];

	struct list_head        clients;

	stats_t                 nbd_stats;

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

	/**
	 * Send structured replies
	 */
	bool                    structured_reply;

	int                     max_used_reqs;
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
void tapdisk_nbdserver_pause(td_nbdserver_t *, bool log);
int tapdisk_nbdserver_unpause(td_nbdserver_t *);

/**
 * Callback to be executed when the client socket becomes ready. It is the core
 * NBD server function that deals with NBD client requests (e.g. I/O read,
 * I/O write, disconnect, etc.).
 */
void tapdisk_nbdserver_clientcb(event_id_t id, char mode, void *data);
int tapdisk_nbdserver_reqs_init(td_nbdserver_client_t *client, int n_reqs);

/**
 * Deallocates the NBD client. If the client has pending requests, the client
 * is not deallocated but simply marked as "dead", the last pending request to
 * complete will actually deallocate it.
 */
void tapdisk_nbdserver_free_client(td_nbdserver_client_t *client);
td_nbdserver_client_t *tapdisk_nbdserver_alloc_client(td_nbdserver_t *server);

/**
 * Tells whether the NBD client is being server by the NBD server.
 */
bool tapdisk_nbdserver_contains_client(td_nbdserver_t *server,
		td_nbdserver_client_t *client);
td_nbdserver_req_t *tapdisk_nbdserver_alloc_request(
		td_nbdserver_client_t *client);
void tapdisk_nbdserver_free_request(td_nbdserver_client_t *client,
		td_nbdserver_req_t *req, bool free_client_if_dead);

/**
 * Tells how many requests are pending.
 */
int tapdisk_nbdserver_reqs_pending(td_nbdserver_client_t *client);

int tapdisk_nbdserver_new_protocol_handshake(td_nbdserver_client_t *client, int);
void tapdisk_nbdserver_handshake_cb(event_id_t, char, void*);

/**
 * Send and receive from the NBD socket
 */
int recv_fully_or_fail(int f, void *buf, size_t len);
int send_fully_or_fail(int f, void *buf, size_t len);

void free_extents(struct tapdisk_extents *extents);

#endif /* _TAPDISK_NBDSERVER_H_ */
