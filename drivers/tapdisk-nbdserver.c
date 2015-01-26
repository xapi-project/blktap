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

#include "debug.h"
#include "tapdisk.h"
#include "tapdisk-log.h"
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
int tapdisk_nbdserver_setup_listening_socket(td_nbdserver_t *server);
int tapdisk_nbdserver_unpause(td_nbdserver_t *server);

td_nbdserver_req_t *
tapdisk_nbdserver_alloc_request(td_nbdserver_client_t *client)
{
	td_nbdserver_req_t *req = NULL;

	if (likely(client->n_reqs_free))
		req = client->reqs_free[--client->n_reqs_free];

	return req;
}

void
tapdisk_nbdserver_free_request(td_nbdserver_client_t *client,
		td_nbdserver_req_t *req)
{
	if (client->n_reqs_free >= client->n_reqs) {
		ERROR("Error, trying to free a client, but the free list "
				"is full! leaking!");
		return;
	}
	client->reqs_free[client->n_reqs_free++] = req;

	if (unlikely(client->dead && !tapdisk_nbdserver_reqs_pending(client)))
		tapdisk_nbdserver_free_client(client);
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

int
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

td_nbdserver_client_t *
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
	client->dead = false;

	return client;

fail:
	if (client) {
		free(client);
		client = NULL;
	}

	return client;
}

void
tapdisk_nbdserver_free_client(td_nbdserver_client_t *client)
{
	INFO("Free client");

	if (!client) {
		ERROR("Attempt to free NULL pointer!");
		return;
	}

	if (client->client_event_id >= 0)
		tapdisk_nbdserver_disable_client(client);

	if (likely(!tapdisk_nbdserver_reqs_pending(client))) {
		list_del(&client->clientlist);
		tapdisk_nbdserver_reqs_free(client);
		free(client);
	} else
		client->dead = true;
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
*get_in_addr(struct sockaddr_storage *ss)
{
	if (ss->ss_family == AF_INET)
		return &(((struct sockaddr_in*)ss)->sin_addr);

	return &(((struct sockaddr_in6*)ss)->sin6_addr);
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
				sent = errno;
				ERROR("Short send/error in callback: %s", strerror(sent));
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

void
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
			rc = errno;
			ERROR("failed to receive from client: %s. Closing connection",
					strerror(rc));
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
		int err = errno;
		close(new_fd);
		if (rc == -1)
			INFO("Short write in negotiation: %s", strerror(err));
		else
			INFO("Short write in negotiation: wrote %d bytes instead of 152\n",
					rc);
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

	INFO("About to accept (server->fdrecv_listening_fd = %d)",
			server->fdrecv_listening_fd);
	new_fd = accept(server->fdrecv_listening_fd,
			(struct sockaddr *)&their_addr, &sin_size);

	if (new_fd == -1) {
		ERROR("failed to accept connection on the fd receiver socket: %s",
				strerror(errno));
		return;
	}

	inet_ntop(their_addr.ss_family, get_in_addr(&their_addr), s, sizeof s);

	/*
	 * FIXME this prints garbage
	 */
	INFO("server: got connection from %s\n", s);

	tapdisk_nbdserver_newclient_fd(server, new_fd);
}

static void
tapdisk_nbdserver_newclient_unix(event_id_t id, char mode, void *data)
{
	int new_fd = 0;
	struct sockaddr_un remote;
	socklen_t t = sizeof(remote);
	td_nbdserver_t *server = data;

	ASSERT(server);

	new_fd = accept(server->unix_listening_fd, (struct sockaddr *)&remote, &t);
	if (new_fd == -1) {
		ERROR("failed to accept connection: %s\n", strerror(errno));
		return;
	}

	INFO("server: got connection\n");

	tapdisk_nbdserver_newclient_fd(server, new_fd);
}

td_nbdserver_t *
tapdisk_nbdserver_alloc(td_vbd_t *vbd, td_disk_info_t info)
{
	td_nbdserver_t *server;
	char fdreceiver_path[TAPDISK_NBDSERVER_MAX_PATH_LEN];
	int err = 0;

	server = malloc(sizeof(*server));
	if (!server) {
		err = errno;
		ERROR("Failed to allocate memory for nbdserver: %s", strerror(err));
		goto out;
	}

	memset(server, 0, sizeof(*server));
	server->fdrecv_listening_fd = -1;
	server->fdrecv_listening_event_id = -1;
	server->unix_listening_fd = -1;
	server->unix_listening_event_id = -1;
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
		goto out;
	}

	if (-1 == snprintf(server->sockpath, TAPDISK_NBDSERVER_MAX_PATH_LEN,
				"%s%d.%d", TAPDISK_NBDSERVER_SOCK_PATH, getpid(), vbd->uuid))
	{
		err = errno;
		ERROR("failed to snprintf %s...%d: %s", TAPDISK_NBDSERVER_SOCK_PATH,
				vbd->uuid, strerror(err));
		goto out;
	}

out:
	if (err) {
		if (server) {
			tapdisk_server_unregister_event(server->fdrecv_listening_event_id);
			close(server->fdrecv_listening_fd);
			free(server);
		}
		return NULL;
	} else
		return server;
}

int
tapdisk_nbdserver_listen_inet(td_nbdserver_t *server, const int port)
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
		if ((server->fdrecv_listening_fd = socket(AF_INET, SOCK_STREAM, 0)) ==
				-1) {
			ERROR("Failed to create socket");
			continue;
		}

		if (setsockopt(server->fdrecv_listening_fd, SOL_SOCKET, SO_REUSEADDR,
					&yes, sizeof(int)) == -1) {
			ERROR("Failed to setsockopt");
			close(server->fdrecv_listening_fd);
			return -1;
		}

		if (bind(server->fdrecv_listening_fd, p->ai_addr, p->ai_addrlen) ==
				-1) {
			ERROR("Failed to bind");
			close(server->fdrecv_listening_fd);
			continue;
		}

		break;
	}

	if (p == NULL) {
		ERROR("Failed to bind");
		close(server->fdrecv_listening_fd);
		return -1;
	}

	freeaddrinfo(servinfo);

	if (listen(server->fdrecv_listening_fd, 10) == -1) {
		ERROR("listen");
		return -1;
	}

	tapdisk_nbdserver_unpause(server);

	if (server->fdrecv_listening_event_id < 0) {
		err = server->fdrecv_listening_event_id;
		close(server->fdrecv_listening_fd);
		return -1;
	}

	INFO("Successfully started NBD server");

	return 0;
}

int
tapdisk_nbdserver_listen_unix(td_nbdserver_t *server)
{
	size_t len = 0;
	int err = 0;

	ASSERT(server);

	ASSERT(server->unix_listening_fd == -1);

	server->unix_listening_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server->unix_listening_fd == -1) {
		err = -errno;
		ERROR("failed to create UNIX domain socket: %s\n", strerror(-err));
		goto out;
    }

	server->local.sun_family = AF_UNIX;
    strcpy(server->local.sun_path, server->sockpath);
    err = unlink(server->local.sun_path);
	if (err == -1 && errno != ENOENT) {
		err = -errno;
		ERROR("failed to remove %s: %s\n", server->local.sun_path,
				strerror(-err));
		goto out;
	}
    len = strlen(server->local.sun_path) + sizeof(server->local.sun_family);
	err = bind(server->unix_listening_fd, (struct sockaddr *)&server->local,
			len);
    if (err == -1) {
		err = -errno;
		ERROR("failed to bind: %s\n", strerror(-err));
		goto out;
    }

	err = listen(server->unix_listening_fd, 10);
	if (err == -1) {
		err = -errno;
		ERROR("failed to listen: %s\n", strerror(-err));
		goto out;
	}

	tapdisk_nbdserver_unpause(server);

	if (server->unix_listening_event_id < 0) {
		ERROR("failed to unpause the NBD server: %s\n",
				strerror(-server->unix_listening_event_id));
		err = server->unix_listening_event_id;
		server->unix_listening_event_id = -1;
		goto out;
	}

	INFO("Successfully started NBD server on %s\n", server->sockpath);

out:
	if (err)
		if (server->unix_listening_fd != -1)
			close(server->unix_listening_fd);
	return err;
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

	if (server->fdrecv_listening_event_id >= 0)
		tapdisk_server_unregister_event(server->fdrecv_listening_event_id);

	if (server->unix_listening_event_id >= 0)
		tapdisk_server_unregister_event(server->unix_listening_event_id);
}

int
tapdisk_nbdserver_unpause(td_nbdserver_t *server)
{
	struct td_nbdserver_client *pos, *q;

	INFO("NBD server unpause(%p) - fdrecv_listening_fd %d, "
			"unix_listening_fd=%d", server,	server->fdrecv_listening_fd,
			server->unix_listening_fd);

	list_for_each_entry_safe(pos, q, &server->clients, clientlist){
		if (pos->paused == 1) {
			tapdisk_nbdserver_enable_client(pos);
			pos->paused = 0;
		}
	}

	if (server->fdrecv_listening_event_id < 0
			&& server->fdrecv_listening_fd >= 0) {
		server->fdrecv_listening_event_id =
			tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					server->fdrecv_listening_fd, 0,
					tapdisk_nbdserver_newclient,
					server);
		INFO("registering for fdrecv_listening_fd");
		if (server->fdrecv_listening_event_id < 0)
			return server->fdrecv_listening_event_id;
	}

	if (server->unix_listening_event_id < 0
			&& server->unix_listening_fd >= 0) {
		server->unix_listening_event_id =
			tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					server->unix_listening_fd, 0,
					tapdisk_nbdserver_newclient_unix,
					server);
		INFO("registering for unix_listening_fd");
		if (server->unix_listening_event_id < 0) {
			tapdisk_server_unregister_event(server->fdrecv_listening_event_id);
			return server->fdrecv_listening_event_id;
		}
	}

	return server->fdrecv_listening_event_id;
}

void
tapdisk_nbdserver_free(td_nbdserver_t *server)
{
	struct td_nbdserver_client *pos, *q;
	int err;

	INFO("NBD server free(%p)", server);

	list_for_each_entry_safe(pos, q, &server->clients, clientlist)
		tapdisk_nbdserver_free_client(pos);

	if (server->fdrecv_listening_event_id >= 0) {
		tapdisk_server_unregister_event(server->fdrecv_listening_event_id);
		server->fdrecv_listening_event_id = -1;
	}

	if (server->fdrecv_listening_fd >= 0) {
		close(server->fdrecv_listening_fd);
		server->fdrecv_listening_fd = -1;
	}

	if (server->fdreceiver)
		td_fdreceiver_stop(server->fdreceiver);

	err = unlink(server->sockpath);
	if (err)
		ERROR("failed to remove UNIX domain socket %s: %s\n", server->sockpath,
				strerror(errno));

	free(server);
}

int
tapdisk_nbdserver_reqs_pending(td_nbdserver_client_t *client) {

	ASSERT(client);

	return client->n_reqs - client->n_reqs_free;
}

bool
tapdisk_nbdserver_contains_client(td_nbdserver_t *server,
		td_nbdserver_client_t *client) {

	td_nbdserver_client_t *_client;

	ASSERT(server);

	list_for_each_entry(_client, &server->clients, clientlist)
		if (client == _client)
			return true;
	return false;
}
