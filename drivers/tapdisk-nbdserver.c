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

#include "timeout-math.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define NBD_SERVER_NUM_REQS TAPDISK_DATA_REQUESTS

/*
 * Server
 */

#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "nbd: " _f, ##_a)
#define ERR(_f, _a...)             tlog_syslog(TLOG_WARN, "nbd: " _f, ##_a)
#define BUG() do {						\
		ERR("Aborting");				\
		abort();					\
	} while (0)
#define BUG_ON(_cond)						\
	if (unlikely(_cond)) {					\
		ERR("(%s) = %d", #_cond, _cond);		\
		BUG();						\
	}

struct td_nbdserver_req {
	td_vbd_request_t        vreq;
	char                    id[16];
	struct td_iovec         iov;
};

td_nbdserver_req_t *
tapdisk_nbdserver_alloc_request(td_nbdserver_client_t *client)
{
	td_nbdserver_req_t *req = NULL;

	ASSERT(client);

	if (likely(client->n_reqs_free))
		req = client->reqs_free[--client->n_reqs_free];

	return req;
}

void
tapdisk_nbdserver_free_request(td_nbdserver_client_t *client,
		td_nbdserver_req_t *req)
{
	ASSERT(client);
	ASSERT(req);
	BUG_ON(client->n_reqs_free >= client->n_reqs);

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

	ASSERT(client);
	ASSERT(n_reqs > 0);

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

static int
tapdisk_nbdserver_enable_client(td_nbdserver_client_t *client)
{
	ASSERT(client);
	ASSERT(client->client_event_id == -1);
	ASSERT(client->client_fd >= 0);

	INFO("Enable client");

	client->client_event_id = tapdisk_server_register_event(
			SCHEDULER_POLL_READ_FD,
			client->client_fd, TV_ZERO,
			tapdisk_nbdserver_clientcb,
			client);

	if (client->client_event_id < 0) {
		ERR("Error registering events on client: %d",
				client->client_event_id);
		return client->client_event_id;
	}

	return client->client_event_id;
}

static void
tapdisk_nbdserver_disable_client(td_nbdserver_client_t *client)
{
	ASSERT(client);
	ASSERT(client->client_event_id >= 0);

	INFO("Disable client");

	tapdisk_server_unregister_event(client->client_event_id);
	client->client_event_id = -1;
}

td_nbdserver_client_t *
tapdisk_nbdserver_alloc_client(td_nbdserver_t *server)
{
	td_nbdserver_client_t *client = NULL;
	int err;

	ASSERT(server);

	INFO("Alloc client");

	client = calloc(1, sizeof(td_nbdserver_client_t));
	if (!client) {
		ERR("Couldn't allocate client structure: %s",
				strerror(errno));
		goto fail;
	}

	err = tapdisk_nbdserver_reqs_init(client, NBD_SERVER_NUM_REQS);
	if (err < 0) {
		ERR("Couldn't allocate client reqs: %d", err);
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
	if (client)
		free(client);

	return NULL;
}

void
tapdisk_nbdserver_free_client(td_nbdserver_client_t *client)
{
	INFO("Free client");

	ASSERT(client);

	if (client->client_event_id >= 0)
		tapdisk_nbdserver_disable_client(client);

	if (likely(!tapdisk_nbdserver_reqs_pending(client))) {
		list_del(&client->clientlist);
		tapdisk_nbdserver_reqs_free(client);
		free(client);
	} else
		client->dead = true;
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
	td_nbdserver_t *server = client->server;
	td_nbdserver_req_t *req = container_of(vreq, td_nbdserver_req_t, vreq);
	unsigned long long interval;
	struct timeval now;
	struct nbd_reply reply;
	int tosend = 0;
	int sent = 0;
	int len = 0;

	reply.magic = htonl(NBD_REPLY_MAGIC);
	reply.error = htonl(error);
	memcpy(reply.handle, req->id, sizeof(reply.handle));

	gettimeofday(&now, NULL);
	interval = timeval_to_us(&now) - timeval_to_us(&vreq->ts);

	if (client->client_fd < 0) {
		ERR("Finishing request for client that has disappeared");
		goto finish;
	}

	send(client->client_fd, &reply, sizeof(reply), 0);

	switch(vreq->op) {
	case TD_OP_READ:
		tosend = len = vreq->iov->secs << SECTOR_SHIFT;
		server->nbd_stats.stats->read_reqs_completed++;
		server->nbd_stats.stats->read_sectors += vreq->iov->secs;
		server->nbd_stats.stats->read_total_ticks += interval;
		while (tosend > 0) {
			sent = send(client->client_fd,
					vreq->iov->base + (len - tosend),
					tosend, 0);
			if (sent <= 0) {
				sent = errno;
				ERR("Short send/error in callback: %s", strerror(sent));
				goto finish;
			}

			tosend -= sent;
		}
		break;
	case TD_OP_WRITE:
		server->nbd_stats.stats->write_reqs_completed++;
		server->nbd_stats.stats->write_sectors += vreq->iov->secs;
		server->nbd_stats.stats->write_total_ticks += interval;
	default:
		break;
	}

	if (error)
		server->nbd_stats.stats->io_errors++;

finish:
	free(vreq->iov->base);
	tapdisk_nbdserver_free_request(client, req);
}

static void
tapdisk_nbdserver_newclient_fd(td_nbdserver_t *server, int new_fd)
{
	td_nbdserver_client_t *client;
	char buffer[256];
	int rc;
	uint64_t tmp64;
	uint32_t tmp32;

	ASSERT(server);
	ASSERT(new_fd >= 0);

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

	if (rc != 152) {
		int err = errno;
		close(new_fd);
		if (rc == -1)
			INFO("Short write in negotiation: %s", strerror(err));
		else
			INFO("Short write in negotiation: wrote %d bytes instead of 152\n",
					rc);
	}

	INFO("About to alloc client");
	client = tapdisk_nbdserver_alloc_client(server);

	INFO("Got an allocated client at %p", client);
	client->client_fd = new_fd;

	INFO("About to enable client");
	if (tapdisk_nbdserver_enable_client(client) < 0) {
		ERR("Error enabling client");
		tapdisk_nbdserver_free_client(client);
		close(new_fd);
	}
}

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
	td_nbdserver_req_t *req;

	req = tapdisk_nbdserver_alloc_request(client);
	if (!req) {
		ERR("Couldn't allocate request in clientcb - killing client");
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
		// FIXME: need to select() on fd with a sensible timeout
		rc = recv(fd, ptr + n, hdrlen - n, 0);
		if (rc == 0) {
			INFO("Client closed connection");
			goto fail;
		}
		if (rc < 0) {
			rc = errno;
			ERR("failed to receive from client: %s. Closing connection",
					strerror(rc));
			goto fail;
		}
		n += rc;
	}

	if (request.magic != htonl(NBD_REQUEST_MAGIC)) {
		ERR("Not enough magic");
		goto fail;
	}

	request.from = ntohll(request.from);
	request.type = ntohl(request.type);
	len = ntohl(request.len);
	if (((len & 0x1ff) != 0) || ((request.from & 0x1ff) != 0)) {
		ERR("Non sector-aligned request (%"PRIu64", %d)",
				request.from, len);
	}

	bzero(req->id, sizeof(req->id));
	memcpy(req->id, request.handle, sizeof(request.handle));

	rc = posix_memalign(&req->iov.base, 512, len);
	if (rc < 0) {
		ERR("posix_memalign failed (%d)", rc);
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
	case TAPDISK_NBD_CMD_READ:
		vreq->op = TD_OP_READ;
                server->nbd_stats.stats->read_reqs_submitted++;
		break;
	case TAPDISK_NBD_CMD_WRITE:
		vreq->op = TD_OP_WRITE;
		server->nbd_stats.stats->write_reqs_submitted++;
		n = 0;
		while (n < len) {
			rc = recv(fd, vreq->iov->base + n, (len - n), 0);
			if (rc <= 0) {
				ERR("Short send or error in "
						"callback: %d", rc);
				goto fail;
			}

			n += rc;
		};

		break;
	case TAPDISK_NBD_CMD_DISC:
		INFO("Received close message. Sending reconnect "
				"header");
		tapdisk_nbdserver_free_client(client);
		INFO("About to send initial connection message");
		tapdisk_nbdserver_newclient_fd(server, fd);
		INFO("Sent");
		return;

	default:
		ERR("Unsupported operation: 0x%x", request.type);
		goto fail;
	}

	rc = tapdisk_vbd_queue_request(server->vbd, vreq);
	if (rc) {
		ERR("tapdisk_vbd_queue_request failed: %d", rc);
		goto fail;
	}

	return;

fail:
	tapdisk_nbdserver_free_client(client);
	return;
}

static void
tapdisk_nbdserver_fdreceiver_cb(int fd, char *msg, void *data)
{
	td_nbdserver_t *server = data;

	ASSERT(server);
	ASSERT(msg);
	ASSERT(fd >= 0);

	INFO("Received fd with msg: %s", msg);

	tapdisk_nbdserver_newclient_fd(server, fd);
}

static void
tapdisk_nbdserver_newclient(event_id_t id, char mode, void *data)
{
	struct sockaddr_storage their_addr;
	socklen_t sin_size = sizeof(their_addr);
	char s[INET6_ADDRSTRLEN+1];
	int new_fd;
	td_nbdserver_t *server = data;

	ASSERT(server);

	INFO("About to accept (server->fdrecv_listening_fd = %d)",
			server->fdrecv_listening_fd);

	new_fd = accept(server->fdrecv_listening_fd,
			(struct sockaddr *)&their_addr, &sin_size);

	if (new_fd == -1) {
		ERR("failed to accept connection on the fd receiver socket: %s",
				strerror(errno));
		return;
	}

	memset(s, 0, sizeof(s));
	inet_ntop(their_addr.ss_family, get_in_addr(&their_addr), s, sizeof(s)-1);

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

	INFO("About to accept (server->unix_listening_fd = %d)",
			server->unix_listening_fd);

	new_fd = accept(server->unix_listening_fd, (struct sockaddr *)&remote, &t);
	if (new_fd == -1) {
		ERR("failed to accept connection: %s\n", strerror(errno));
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

	server = calloc(1, sizeof(*server));
	if (!server) {
		ERR("Failed to allocate memory for nbdserver: %s",
				strerror(errno));
		goto fail;
	}

	server->vbd = vbd;
	server->info = info;
	server->fdrecv_listening_fd = -1;
	server->fdrecv_listening_event_id = -1;
	server->unix_listening_fd = -1;
	server->unix_listening_event_id = -1;
	INIT_LIST_HEAD(&server->clients);

	if (td_metrics_nbd_start(&server->nbd_stats, server->vbd->tap->minor)) {
		ERR("failed to create metrics file for nbdserver");
		goto fail;
	}

	if (snprintf(fdreceiver_path, TAPDISK_NBDSERVER_MAX_PATH_LEN,
			"%s%d.%d", TAPDISK_NBDSERVER_LISTEN_SOCK_PATH, getpid(),
			vbd->uuid) < 0) {
		ERR("Failed to snprintf fdreceiver_path");
		goto fail;
	}

	server->fdreceiver = td_fdreceiver_start(fdreceiver_path,
			tapdisk_nbdserver_fdreceiver_cb, server);
	if (!server->fdreceiver) {
		ERR("Error setting up fd receiver");
		goto fail;
	}

	if (snprintf(server->sockpath, TAPDISK_NBDSERVER_MAX_PATH_LEN,
			"%s%d.%d", TAPDISK_NBDSERVER_SOCK_PATH, getpid(),
			vbd->uuid) < 0) {
		ERR("Failed to snprintf sockpath");
		goto fail;
	}

	return server;

fail:
	if (server) {
		if (server->fdreceiver)
			td_fdreceiver_stop(server->fdreceiver);
		free(server);
	}

	return NULL;
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

	if (server->fdrecv_listening_event_id >= 0) {
		tapdisk_server_unregister_event(server->fdrecv_listening_event_id);
		server->fdrecv_listening_event_id = -1;
	}

	if (server->unix_listening_event_id >= 0) {
		tapdisk_server_unregister_event(server->unix_listening_event_id);
		server->unix_listening_event_id = -1;
	}
}

static int
tapdisk_nbdserver_unpause_fdrecv(td_nbdserver_t *server)
{
	int err = 0;

	ASSERT(server);

	if (server->fdrecv_listening_event_id < 0
			&& server->fdrecv_listening_fd >= 0) {
		INFO("registering for fdrecv_listening_fd");
		server->fdrecv_listening_event_id =
			tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					server->fdrecv_listening_fd, TV_ZERO,
					tapdisk_nbdserver_newclient,
					server);
		if (server->fdrecv_listening_event_id < 0) {
			err = server->fdrecv_listening_event_id;
			server->fdrecv_listening_event_id = -1;
		}
	}

	return err;
}

static int
tapdisk_nbdserver_unpause_unix(td_nbdserver_t *server)
{
	int err = 0;

	ASSERT(server);

	if (server->unix_listening_event_id < 0
			&& server->unix_listening_fd >= 0) {
		INFO("registering for unix_listening_fd");
		server->unix_listening_event_id =
			tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					server->unix_listening_fd, TV_ZERO,
					tapdisk_nbdserver_newclient_unix,
					server);
		if (server->unix_listening_event_id < 0) {
			err = server->unix_listening_event_id;
			server->unix_listening_event_id = -1;
		}
	}

	return err;
}

int
tapdisk_nbdserver_listen_inet(td_nbdserver_t *server, const int port)
{
	struct addrinfo hints, *servinfo, *p;
	char portstr[10];
	int err;
	int yes = 1;

	ASSERT(server);
	ASSERT(server->fdrecv_listening_fd == -1);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	snprintf(portstr, 10, "%d", port);

	err = getaddrinfo(NULL, portstr, &hints, &servinfo);
	if (err) {
		if (err == EAI_SYSTEM) {
			ERR("Failed to getaddrinfo: %s", strerror(errno));
			return -errno;
		} else {
			ERR("Failed to getaddrinfo: %s", gai_strerror(err));
			return -1;
		}
	}

	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((server->fdrecv_listening_fd = socket(AF_INET, SOCK_STREAM, 0)) ==
				-1) {
			ERR("Failed to create socket");
			continue;
		}

		if (setsockopt(server->fdrecv_listening_fd, SOL_SOCKET, SO_REUSEADDR,
					&yes, sizeof(int)) == -1) {
			ERR("Failed to setsockopt");
			close(server->fdrecv_listening_fd);
			continue;
		}

		if (bind(server->fdrecv_listening_fd, p->ai_addr, p->ai_addrlen) ==
				-1) {
			ERR("Failed to bind");
			close(server->fdrecv_listening_fd);
			continue;
		}

		break;
	}

	freeaddrinfo(servinfo);

	if (p == NULL) {
		ERR("Failed to bind");
		err = -1;
		goto out;
	}

	err = listen(server->fdrecv_listening_fd, 10);
	if (err) {
		err = -errno;
		ERR("listen: %s", strerror(-err));
		goto out;
	}

	err = tapdisk_nbdserver_unpause_fdrecv(server);
	if (err) {
		ERR("failed to unpause the NBD server (fdrecv): %s\n",
				strerror(-err));
		goto out;
	}

	INFO("Successfully started NBD server (fdrecv)");

out:
	if (err && (server->fdrecv_listening_fd != -1)) {
		close(server->fdrecv_listening_fd);
		server->fdrecv_listening_fd = -1;
	}
	return err;
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
		ERR("failed to create UNIX domain socket: %s\n", strerror(-err));
		goto out;
	}

	server->local.sun_family = AF_UNIX;
	strcpy(server->local.sun_path, server->sockpath);
	err = unlink(server->local.sun_path);
	if (err == -1 && errno != ENOENT) {
		err = -errno;
		ERR("failed to remove %s: %s\n", server->local.sun_path,
				strerror(-err));
		goto out;
	}
	len = strlen(server->local.sun_path) + sizeof(server->local.sun_family);
	err = bind(server->unix_listening_fd, (struct sockaddr *)&server->local,
			len);
	if (err == -1) {
		err = -errno;
		ERR("failed to bind: %s\n", strerror(-err));
		goto out;
	}

	err = listen(server->unix_listening_fd, 10);
	if (err == -1) {
		err = -errno;
		ERR("failed to listen: %s\n", strerror(-err));
		goto out;
	}

	err = tapdisk_nbdserver_unpause_unix(server);
	if (err) {
		ERR("failed to unpause the NBD server (unix): %s\n",
				strerror(-err));
		goto out;
	}

	INFO("Successfully started NBD server on %s\n", server->sockpath);

out:
	if (err && (server->unix_listening_fd != -1)) {
		close(server->unix_listening_fd);
		server->unix_listening_fd = -1;
	}
	return err;
}

int
tapdisk_nbdserver_unpause(td_nbdserver_t *server)
{
	struct td_nbdserver_client *pos, *q;
	int err;

	ASSERT(server);

	INFO("NBD server unpause(%p) - fdrecv_listening_fd %d, "
			"unix_listening_fd=%d", server,	server->fdrecv_listening_fd,
			server->unix_listening_fd);

	list_for_each_entry_safe(pos, q, &server->clients, clientlist){
		if (pos->paused == 1) {
			tapdisk_nbdserver_enable_client(pos);
			pos->paused = 0;
		}
	}

	err = tapdisk_nbdserver_unpause_fdrecv(server);
	if (err)
		return err;

	err = tapdisk_nbdserver_unpause_unix(server);
	if (err)
		return err;

	return 0;
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

	if (server->unix_listening_event_id >= 0) {
		tapdisk_server_unregister_event(server->unix_listening_event_id);
		server->unix_listening_event_id = -1;
	}

	if (server->unix_listening_fd >= 0) {
		close(server->unix_listening_fd);
		server->unix_listening_fd = -1;
	}

	err = unlink(server->sockpath);
	if (err)
		ERR("failed to remove UNIX domain socket %s: %s\n", server->sockpath,
				strerror(errno));
	err = td_metrics_nbd_stop(&server->nbd_stats);

	if (err)
		ERR("failed to delete NBD metrics: %s\n", strerror(errno));

	free(server);
}

int
tapdisk_nbdserver_reqs_pending(td_nbdserver_client_t *client)
{
	ASSERT(client);

	return client->n_reqs - client->n_reqs_free;
}

bool
tapdisk_nbdserver_contains_client(td_nbdserver_t *server,
		td_nbdserver_client_t *client)
{
	td_nbdserver_client_t *_client;

	ASSERT(server);

	list_for_each_entry(_client, &server->clients, clientlist)
		if (client == _client)
			return true;
	return false;
}
