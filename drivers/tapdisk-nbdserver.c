/*
 * Copyright (c) 2012, Citrix Systems, Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/un.h>

#include "tapdisk.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-utils.h"
#include "tapdisk-nbdserver.h"
#include "tapdisk-fdreceiver.h"

#include "tapdisk-nbd.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define NBD_SERVER_NUM_REQS TAPDISK_DATA_REQUESTS

#define TAPDISK_NBDSERVER_LISTEN_SOCK_PATH "/var/run/blktap-control/nbdserver"
#define TAPDISK_NBDSERVER_MAX_PATH_LEN 256

/*
 * Server 
 */

#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "nbd: " _f, ##_a)
#define ERROR(_f, _a...)           tlog_syslog(TLOG_WARN, "nbd: " _f, ##_a)

struct td_nbdserver_req {
	td_vbd_request_t        vreq;
	char                    id[16];
	struct td_iovec         iov;
};

static void tapdisk_nbdserver_disable_client(td_nbdserver_client_t *client);
static void tapdisk_nbdserver_clientcb(event_id_t id, char mode, void *data);
int tapdisk_nbdserver_setup_listening_socket(td_nbdserver_t *server);
int tapdisk_nbdserver_unpause(td_nbdserver_t *server);

static td_nbdserver_req_t *
tapdisk_nbdserver_alloc_request(td_nbdserver_client_t *client)
{
	td_nbdserver_req_t *req = NULL;

	if (likely(client->n_reqs_free))
		req = client->reqs_free[--client->n_reqs_free];

	return req;
}

static void
tapdisk_nbdserver_free_request(td_nbdserver_client_t *client,
		td_nbdserver_req_t *req)
{
	if (client->n_reqs_free >= client->n_reqs) {
		ERROR("Error, trying to free a client, but the free list "
				"is full! leaking!");
		return;
	}
	client->reqs_free[client->n_reqs_free++] = req;
}

static void
tapdisk_nbdserver_reqs_free(td_nbdserver_client_t *client)
{
	if (client->reqs) {
		free(client->reqs);
		client->reqs = NULL;
	}

	if (client->iovecs) {
		free(client->iovecs);
		client->iovecs = NULL;
	}

	if (client->reqs_free) {
		free(client->reqs_free);
		client->reqs_free = NULL;
	}
}

static int
tapdisk_nbdserver_reqs_init(td_nbdserver_client_t *client, int n_reqs)
{
	int i, err;

	INFO("Reqs init");

	client->reqs = malloc(n_reqs * sizeof(td_nbdserver_req_t));
	if (!client->reqs) {
		err = -errno;
		goto fail;
	}
	client->iovecs = malloc(n_reqs * sizeof(struct td_iovec));
	if (!client->iovecs) {
		err = - errno;
		goto fail;
	}

	client->reqs_free = malloc(n_reqs * sizeof(td_nbdserver_req_t*));
	if (!client->reqs_free) {
		err = -errno;
		goto fail;
	}

	client->n_reqs      = n_reqs;
	client->n_reqs_free = 0;

	for (i = 0; i < n_reqs; i++) {
		client->reqs[i].vreq.iov = &client->iovecs[i];
		tapdisk_nbdserver_free_request(client, &client->reqs[i]);
	}

	return 0;

fail:
	tapdisk_nbdserver_reqs_free(client);
	return err;
}

static td_nbdserver_client_t *
tapdisk_nbdserver_alloc_client(td_nbdserver_t *server)
{
	td_nbdserver_client_t *client = NULL;
	int err;

	INFO("Alloc client");

	client = malloc(sizeof(td_nbdserver_client_t));
	if (!client) {
		ERROR("Couldn't allocate client structure: %s",
				strerror(errno));
		goto fail;
	}

	bzero(client, sizeof(td_nbdserver_client_t));

	err = tapdisk_nbdserver_reqs_init(client, NBD_SERVER_NUM_REQS);
	if (err < 0) {
		ERROR("Couldn't allocate client reqs: %d", err);
		goto fail;
	}

	client->client_fd = -1;
	client->client_event_id = -1;
	client->server = server;
	INIT_LIST_HEAD(&client->clientlist);
	list_add(&client->clientlist, &server->clients);

	client->paused = 0;

	return client;

fail:
	if (client) {
		free(client);
		client = NULL;
	}

	return client;
}

static void
tapdisk_nbdserver_free_client(td_nbdserver_client_t *client)
{
	INFO("Free client");

	if (!client) {
		ERROR("Attempt to free NULL pointer!");
		return;
	}

	if (client->client_event_id >= 0)
		tapdisk_nbdserver_disable_client(client);

	list_del(&client->clientlist);
	tapdisk_nbdserver_reqs_free(client);
	free(client);
}

static int 
tapdisk_nbdserver_enable_client(td_nbdserver_client_t *client)
{
	INFO("Enable client");

	if (client->client_event_id >= 0) {
		ERROR("Attempting to enable an already-enabled client");
		return -1;
	}

	if (client->client_fd < 0) {
		ERROR("Attempting to register events on a closed client");
		return -1;
	}

	client->client_event_id = tapdisk_server_register_event(
			SCHEDULER_POLL_READ_FD,
			client->client_fd, 0,
			tapdisk_nbdserver_clientcb,
			client);	

	if (client->client_event_id < 0) {
		ERROR("Error registering events on client: %d",
				client->client_event_id);
		return client->client_event_id;
	}

	return client->client_event_id;
}

static void
tapdisk_nbdserver_disable_client(td_nbdserver_client_t *client)
{
	INFO("Disable client");

	if (client->client_event_id < 0) {
		ERROR("Attempting to disable an already-disabled client");
		return;
	}

	tapdisk_server_unregister_event(client->client_event_id);
	client->client_event_id = -1;
}

static void
*get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET)
		return &(((struct sockaddr_in*)sa)->sin_addr);

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

static void
__tapdisk_nbdserver_request_cb(td_vbd_request_t *vreq, int error,
		void *token, int final)
{
	td_nbdserver_client_t *client = token;
	td_nbdserver_req_t *req = containerof(vreq, td_nbdserver_req_t, vreq);
	struct nbd_reply reply;
	int tosend = 0;
	int sent = 0;
	int len = 0;

	reply.magic = htonl(NBD_REPLY_MAGIC);
	reply.error = htonl(error);
	memcpy(reply.handle, req->id, sizeof(reply.handle));

	if (client->client_fd < 0) {
		ERROR("Finishing request for client that has disappeared");
		goto finish;
	}

	send(client->client_fd, &reply, sizeof(reply), 0);

	switch(vreq->op) {
	case TD_OP_READ:
		tosend = len = vreq->iov->secs << SECTOR_SHIFT;
		while (tosend > 0) {
			sent = send(client->client_fd,
					vreq->iov->base + (len - tosend),
					tosend, 0);
			if (sent <= 0) {
				ERROR("Short send or error in "
						"callback: %d", sent);
				goto finish;
			}

			tosend -= sent;
		}
		break;
	default:
		break;
	}

finish:
	free(vreq->iov->base);
	tapdisk_nbdserver_free_request(client, req);
}

static void tapdisk_nbdserver_newclient_fd(td_nbdserver_t *server, int new_fd);

static void
tapdisk_nbdserver_clientcb(event_id_t id, char mode, void *data)
{
	td_nbdserver_client_t *client = data;
	td_nbdserver_t *server = client->server;
	int rc;
	int len;
	int hdrlen;
	int n;
	int fd = client->client_fd;
	char *ptr;
	td_vbd_request_t *vreq;
	struct nbd_request request;

	td_nbdserver_req_t *req = tapdisk_nbdserver_alloc_request(client);

	if (req == NULL) {
		ERROR("Couldn't allocate request in clientcb - killing client");
		tapdisk_nbdserver_free_client(client);
		return;
	}

	vreq = &req->vreq;

	memset(req, 0, sizeof(td_nbdserver_req_t));
	/* Read the request the client has sent */

	hdrlen = sizeof(struct nbd_request);

	n = 0;
	ptr = (char *) &request;
	while (n < hdrlen) {
		rc = recv(fd, ptr + n, hdrlen - n, 0);
		if (rc == 0) {
			INFO("Client closed connection");
			goto fail;
		}
		if (rc < 0) {
			ERROR("Bad return in nbdserver_clientcb. Closing "
					"connection");
			goto fail;
		}
		n += rc;
	}

	if (request.magic != htonl(NBD_REQUEST_MAGIC)) {
		ERROR("Not enough magic");
		goto fail;
	}

	request.from = ntohll(request.from);
	request.type = ntohl(request.type);
	len = ntohl(request.len);
	if (((len & 0x1ff) != 0) || ((request.from & 0x1ff) != 0)) {
		ERROR("Non sector-aligned request (%"PRIu64", %d)",
				request.from, len);
	}

	bzero(req->id, sizeof(req->id));
	memcpy(req->id, request.handle, sizeof(request.handle));

	rc = posix_memalign(&req->iov.base, 512, len);
	if (rc < 0) {
		ERROR("posix_memalign failed (%d)", rc);
		goto fail;
	}

	vreq->sec = request.from >> SECTOR_SHIFT;
	vreq->iovcnt = 1;
	vreq->iov = &req->iov;
	vreq->iov->secs = len >> SECTOR_SHIFT;
	vreq->token = client;
	vreq->cb = __tapdisk_nbdserver_request_cb;
	vreq->name = req->id;
	vreq->vbd = server->vbd;

	switch(request.type) {
	case NBD_CMD_READ:
		vreq->op = TD_OP_READ;
		break;
	case NBD_CMD_WRITE:
		vreq->op = TD_OP_WRITE;

		n = 0;
		while (n < len) {
			rc = recv(fd, vreq->iov->base + n, (len - n), 0);
			if (rc <= 0) {
				ERROR("Short send or error in "
						"callback: %d", rc);
				goto fail;
			}

			n += rc;
		};

		break;
	case NBD_CMD_DISC:
		INFO("Received close message. Sending reconnect "
				"header");
		tapdisk_nbdserver_free_client(client);
		INFO("About to send initial connection message");
		tapdisk_nbdserver_newclient_fd(server, fd);
		INFO("Sent");
		return;

	default:
		ERROR("Unsupported operation: 0x%x", request.type);
		goto fail;
	}

	rc = tapdisk_vbd_queue_request(server->vbd, vreq);
	if (rc) {
		ERROR("tapdisk_vbd_queue_request failed: %d", rc);
		goto fail;
	}

	return;

fail:
	tapdisk_nbdserver_free_client(client);
	return;
}

static void
tapdisk_nbdserver_newclient_fd(td_nbdserver_t *server, int new_fd)
{
	char buffer[256];
	int rc;
	uint64_t tmp64;
	uint32_t tmp32;

	INFO("Got a new client!");

	/* Spit out the NBD connection stuff */

	memcpy(buffer, "NBDMAGIC", 8);
	tmp64 = htonll(NBD_NEGOTIATION_MAGIC);
	memcpy(buffer + 8, &tmp64, sizeof(tmp64));
	tmp64 = htonll(server->info.size * server->info.sector_size);
	memcpy(buffer + 16, &tmp64, sizeof(tmp64));
	tmp32 = htonl(0);
	memcpy(buffer + 24, &tmp32, sizeof(tmp32));
	bzero(buffer + 28, 124);

	rc = send(new_fd, buffer, 152, 0);

	if (rc < 152) {
		close(new_fd);
		INFO("Short write in negotiation!");
	}	

	INFO("About to alloc client");
	td_nbdserver_client_t *client = tapdisk_nbdserver_alloc_client(server);
	INFO("Got an allocated client at %p", client);
	client->client_fd = new_fd;
	INFO("About to enable client");

	if (tapdisk_nbdserver_enable_client(client) < 0) {
		ERROR("Error enabling client");
		tapdisk_nbdserver_free_client(client);
		close(new_fd);
		return;
	}
}

static void 
tapdisk_nbdserver_fdreceiver_cb(int fd, char *msg, void *data)
{
	td_nbdserver_t *server = data;
	INFO("Received fd with msg: %s", msg);
	tapdisk_nbdserver_newclient_fd(server, fd);
}

static void
tapdisk_nbdserver_newclient(event_id_t id, char mode, void *data)
{
	struct sockaddr_storage their_addr;
	socklen_t sin_size = sizeof(their_addr);
	char s[INET6_ADDRSTRLEN];
	int new_fd;
	td_nbdserver_t *server = data;

	INFO("About to accept (server->listening_fd = %d)",
			server->listening_fd);
	new_fd = accept(server->listening_fd, (struct sockaddr *)&their_addr,
			&sin_size);

	if (new_fd == -1) {
		ERROR("accept (%s)", strerror(errno));
		return;
	}

	inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr),
			s, sizeof s);

	INFO("server: got connection from %s\n", s);

	tapdisk_nbdserver_newclient_fd(server, new_fd);
}

td_nbdserver_t *
tapdisk_nbdserver_alloc(td_vbd_t *vbd, td_disk_info_t info)
{
	td_nbdserver_t *server;
	char fdreceiver_path[TAPDISK_NBDSERVER_MAX_PATH_LEN];

	server = malloc(sizeof(*server));
	if (!server) {
		ERROR("Failed to allocate memory for nbdserver, errno = %d",
				errno);
		return NULL;
	}

	memset(server, 0, sizeof(*server));
	server->listening_fd = -1;
	server->listening_event_id = -1;
	INIT_LIST_HEAD(&server->clients);

	server->vbd = vbd;
	server->info = info;

	snprintf(fdreceiver_path, TAPDISK_NBDSERVER_MAX_PATH_LEN, "%s%d.%d",
			TAPDISK_NBDSERVER_LISTEN_SOCK_PATH, getpid(), 
			vbd->uuid);

	server->fdreceiver = td_fdreceiver_start(fdreceiver_path, 
			tapdisk_nbdserver_fdreceiver_cb, server);

	if (!server->fdreceiver) {
		ERROR("Error setting up fd receiver");
		tapdisk_server_unregister_event(server->listening_event_id);
		close(server->listening_fd);
		return NULL;
	}

	return server;
}

int
tapdisk_nbdserver_listen(td_nbdserver_t *server, int port)
{
	struct addrinfo hints, *servinfo, *p;
	char portstr[10];
	int err;
	int yes = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	snprintf(portstr, 10, "%d", port);

	if ((err = getaddrinfo(NULL, portstr, &hints, &servinfo)) != 0) {
		ERROR("Failed to getaddrinfo");
		return -1;
	}

	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((server->listening_fd = socket(AF_INET, SOCK_STREAM, 0)) ==
				-1) {
			ERROR("Failed to create socket");
			continue;
		}

		if (setsockopt(server->listening_fd, SOL_SOCKET, SO_REUSEADDR,
					&yes, sizeof(int)) == -1) {
			ERROR("Failed to setsockopt");
			close(server->listening_fd);
			return -1;
		}

		if (bind(server->listening_fd, p->ai_addr, p->ai_addrlen) ==
				-1) {
			ERROR("Failed to bind");
			close(server->listening_fd);
			continue;
		}

		break;
	}

	if (p == NULL) {
		ERROR("Failed to bind");
		close(server->listening_fd);
		return -1;
	}

	freeaddrinfo(servinfo);

	if (listen(server->listening_fd, 10) == -1) {
		ERROR("listen");
		return -1;
	}

	tapdisk_nbdserver_unpause(server);

	if (server->listening_event_id < 0) {
		err = server->listening_event_id;
		close(server->listening_fd);
		return -1;
	}

	INFO("Successfully started NBD server");

	return 0;
}

void
tapdisk_nbdserver_pause(td_nbdserver_t *server)
{
	struct td_nbdserver_client *pos, *q;

	INFO("NBD server pause(%p)", server);

	list_for_each_entry_safe(pos, q, &server->clients, clientlist){
		if (pos->paused != 1 && pos->client_event_id >= 0) { 
			tapdisk_nbdserver_disable_client(pos);
			pos->paused = 1;
		}
	}

	if (server->listening_event_id >= 0)
		tapdisk_server_unregister_event(server->listening_event_id);
}

int 
tapdisk_nbdserver_unpause(td_nbdserver_t *server)
{
	struct td_nbdserver_client *pos, *q;

	INFO("NBD server unpause(%p) - listening_fd = %d", server,
			server->listening_fd);

	list_for_each_entry_safe(pos, q, &server->clients, clientlist){
		if (pos->paused == 1) {
			tapdisk_nbdserver_enable_client(pos);
			pos->paused = 0;
		}
	}

	if (server->listening_event_id < 0 && server->listening_fd >= 0) {
		server->listening_event_id =
			tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					server->listening_fd, 0,
					tapdisk_nbdserver_newclient,
					server);	
		INFO("registering for listening_fd");
	}

	return server->listening_event_id;
}

void 
tapdisk_nbdserver_free(td_nbdserver_t *server)
{
	struct td_nbdserver_client *pos, *q;

	INFO("NBD server free(%p)", server);

	list_for_each_entry_safe(pos, q, &server->clients, clientlist)
		tapdisk_nbdserver_free_client(pos);

	if (server->listening_event_id >= 0) {
		tapdisk_server_unregister_event(server->listening_event_id);
		server->listening_event_id = -1;
	}

	if (server->listening_fd >= 0) {
		close(server->listening_fd);
		server->listening_fd = -1;
	}

	if (server->fdreceiver)
		td_fdreceiver_stop(server->fdreceiver);

	free(server);	
}
