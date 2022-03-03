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
#include "tapdisk-protocol-new.h"
#include <byteswap.h>

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

#define MAX_OPTIONS 32

#define NBD_SERVER_NUM_REQS TAPDISK_DATA_REQUESTS
#define MAX_REQUEST_SIZE (64 * 1024 * 1024)

uint16_t gflags = (NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);
static const int SERVER_USE_OLD_PROTOCOL = 1;

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

void
free_extents(struct tapdisk_extents *extents)
{
	tapdisk_extent_t *next, *curr_extent = extents->head;
	while (curr_extent) {
		next = curr_extent->next;
		free(curr_extent);
		curr_extent = next;
	}

	free(extents);
}

struct nbd_block_descriptor *
convert_extents_to_block_descriptors (struct tapdisk_extents *extents)
{
    size_t i, nr_extents = extents->count;
    struct nbd_block_descriptor *blocks;
    tapdisk_extent_t *curr_extent = extents->head;
    
    blocks = calloc (nr_extents, sizeof (struct nbd_block_descriptor));
    if(blocks == NULL) {
	    return NULL;
    }

    for (i = 0; i < nr_extents; ++i) {
      blocks[i].length = htobe32 ((curr_extent->length << SECTOR_SHIFT));
      blocks[i].status_flags = htobe32(curr_extent->flag);
      curr_extent = curr_extent->next;
    }
    return blocks;
}

static int
send_structured_reply_block_status (int fd, char* id, struct tapdisk_extents *extents)
{
	struct nbd_structured_reply reply;
	struct nbd_block_descriptor *blocks;
	size_t i, nr_blocks = extents->count;
	uint32_t context_id;
	int ret = 0;

	blocks = convert_extents_to_block_descriptors (extents);
	if(blocks == NULL) {
		ERR("Could not allocate blocks for extents");
		ret = -1;
		goto done;
	}	

	reply.magic = htobe32 (NBD_STRUCTURED_REPLY_MAGIC);
	memcpy(&reply.handle, id, sizeof(reply.handle));
	reply.flags = htobe16 (NBD_REPLY_FLAG_DONE);
	reply.type = htobe16 (NBD_REPLY_TYPE_BLOCK_STATUS);
	reply.length = htobe32 (sizeof context_id +
				nr_blocks * sizeof (struct nbd_block_descriptor));

	int rc = send (fd, &reply, sizeof(reply), 0);
	if(rc != sizeof(reply)) {
		ERR("Could  not send stuctured reply struct");
		ret = -errno;
		goto done;
	}	

	context_id = htobe32 (base_allocation_id);
	rc = send (fd, &context_id, sizeof(context_id), 0);
	if(rc != sizeof(context_id)) {
		ERR("Could  not send contect_id");
		ret = -errno;
		goto done;
	}	

	for (i = 0; i < nr_blocks; ++i) {
		rc = send (fd, &blocks[i], sizeof blocks[i], 0);
		if(rc != sizeof(blocks[i])) {
			ERR("Could not send extent block");
			ret = -errno;
			goto done;
		}	
	}
done:
	free(blocks);
	return ret;   
}

td_nbdserver_req_t *
tapdisk_nbdserver_alloc_request(td_nbdserver_client_t *client)
{
	td_nbdserver_req_t *req = NULL;

	ASSERT(client);

	if (likely(client->n_reqs_free))
		req = client->reqs_free[--client->n_reqs_free];

	return req;
}

static int
send_option_reply (int new_fd, uint32_t option, uint32_t reply)
{
	struct nbd_fixed_new_option_reply fixed_new_option_reply;

	fixed_new_option_reply.magic = htobe64 (NBD_REP_MAGIC);
	fixed_new_option_reply.option = htobe32 (option);
	fixed_new_option_reply.reply = htobe32 (reply);
	fixed_new_option_reply.replylen = htobe32 (0);

	int rc = send(new_fd, &fixed_new_option_reply, sizeof(fixed_new_option_reply), 0);
	if(rc != sizeof(fixed_new_option_reply)) {
		ERR("Failed to send new_option_reply");
		return -1; 
	}
	return 0;
}

static void
tapdisk_nbdserver_set_free_request(td_nbdserver_client_t *client,
		td_nbdserver_req_t *req)
{
	ASSERT(client);
	ASSERT(req);
	BUG_ON(client->n_reqs_free >= client->n_reqs);

	client->reqs_free[client->n_reqs_free++] = req;
}

void
tapdisk_nbdserver_free_request(td_nbdserver_client_t *client,
		td_nbdserver_req_t *req)
{
	tapdisk_nbdserver_set_free_request(client, req);
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

void
provide_server_info (td_nbdserver_t *server, uint64_t *exportsize, uint16_t *flags)
{
	int64_t size;
	uint16_t eflags = NBD_FLAG_HAS_FLAGS;

	size = server->info.size * server->info.sector_size;

	*exportsize = size;
	*flags = eflags;
}

static int 
send_info_export (int new_fd, uint32_t option, uint32_t reply, uint16_t info, uint64_t exportsize,
		  uint16_t flags)
{
	struct nbd_fixed_new_option_reply fixed_new_option_reply;
	struct nbd_fixed_new_option_reply_info_export export;

	fixed_new_option_reply.magic = htobe64 (NBD_REP_MAGIC);
	fixed_new_option_reply.option = htobe32 (option);
	fixed_new_option_reply.reply = htobe32 (reply);
	fixed_new_option_reply.replylen = htobe32 (sizeof export);

	export.info = htobe16 (info);
	export.exportsize = htobe64 (exportsize);
	export.eflags = htobe16 (flags);

	int rc = send(new_fd, &fixed_new_option_reply, sizeof(fixed_new_option_reply), 0);
	if(rc != sizeof(fixed_new_option_reply)) {
		ERR("Failed to send new_option_reply");
		return -1; 
	}

	rc = send(new_fd, &export, sizeof(export), 0);
	if(rc != sizeof(export)) {
		ERR("Failed to send info export");
		return -1; 
	}

	return 0;
}

int
send_meta_context (int new_fd, uint32_t reply, uint32_t context_id, const char *name)
{
	struct nbd_fixed_new_option_reply fixed_new_option_reply;
	struct nbd_fixed_new_option_reply_meta_context context;
	const size_t namelen = strlen (name);

	fixed_new_option_reply.magic = htobe64 (NBD_REP_MAGIC);
	fixed_new_option_reply.option = htobe32 (NBD_OPT_SET_META_CONTEXT);
	fixed_new_option_reply.reply = htobe32 (reply);
	fixed_new_option_reply.replylen = htobe32 (sizeof context + namelen);
	context.context_id = htobe32 (context_id);

	int rc = send (new_fd, &fixed_new_option_reply, sizeof(fixed_new_option_reply), 0);
	if(rc != sizeof(fixed_new_option_reply)) {
		ERR("Failed to send new_option_reply");
		return -1; 
	}
	rc = send (new_fd, &context, sizeof(context), 0);
	if(rc != sizeof(fixed_new_option_reply)) {
		ERR("Failed to send context");
		return -1; 
	}
	rc = send (new_fd, name, namelen, 0);
	if(rc != sizeof(fixed_new_option_reply)) {
		ERR("Failed to send name");
		return -1; 
	}

	return 0;
}

int
receive_info(int fd, char* buf, int size)
{
	int n = 0;
	int hdrlen = size;

	while (n < size) {
		int rc = recv(fd, buf + n, hdrlen - n, 0);
		if (rc == 0) {
			ERR("Connection was closed by client");
			return -1;
		}
		if (rc < 0) {
			rc = errno;
			ERR("Client caused following error %s", strerror(rc));
			return -rc;
		}
		n += rc;
	}

	return 0;
}

static int
receive_newstyle_options(td_nbdserver_t *server, int new_fd, bool no_zeroes)
{
	struct nbd_new_option n_option;
	size_t n_options;
	uint32_t opt_code;
	uint32_t opt_len;
	uint64_t opt_version;
	uint64_t exportsize;
	struct nbd_export_name_option_reply handshake_finish;
	char *buf = NULL;
	int ret = 0;

	for (n_options = 0; n_options < MAX_OPTIONS; n_options++) {

		if(receive_info(new_fd, (char *)&n_option, sizeof(n_option)) == -1){
			return -1;	
		}

		opt_version = be64toh(n_option.version);
		if (NBD_OPT_MAGIC != opt_version){
			ERR("Bad NBD option version %" PRIx64
			    ", expected %" PRIx64,
                            opt_version, NBD_OPT_MAGIC);
			ret = -1;	
			goto done;
		}

		opt_len = be32toh (n_option.optlen);
		if (opt_len > MAX_REQUEST_SIZE) {
			ERR ("NBD optlen to big (%" PRIu32 ")", opt_len);
			ret = -1;	
			goto done;
		}

		buf = malloc (opt_len + 1); 
		if (buf == NULL) {
			ERR("Could not malloc option data buff");
			ret = -1;	
			goto done;
		}

		opt_code = be32toh (n_option.option);

		switch (opt_code) {
		case NBD_OPT_EXPORT_NAME:
		{
			INFO("Processing NBD_OPT_EXPORT_NAME");
			uint16_t flags = 0;
			if(receive_info(new_fd, (char *)buf, opt_len) == -1){
				ERR ("Failed to received data for NBD_OPT_EXPORT_NAME");
				ret = -1;	
				goto done;
			}
			buf[opt_len] = '\0';
			INFO("Exportname \"%s\"", buf);

			provide_server_info(server, &exportsize, &flags);

			bzero(&handshake_finish, sizeof handshake_finish);
			handshake_finish.exportsize = htobe64 (exportsize);
      			handshake_finish.eflags = htobe16 (flags);
			ssize_t len = no_zeroes ? 10 : sizeof(handshake_finish);
			ssize_t sent = send (new_fd, &handshake_finish,len, 0);
			if(sent != len) {
				ERR ("Failed to send handshake finish");
				ret = -1;	
				goto done;
			}
		}
		break;
		case NBD_OPT_ABORT:
			ERR("NBD_OPT_ABORT: not implemented");
			break;
		case NBD_OPT_LIST:
			ERR("NBD_OPT_LIST: not implemented");
			break;
		case NBD_OPT_STARTTLS:
			ERR("NBD_OPT_STARTTLS: not implemented");
			break;
		case NBD_OPT_INFO:
			ERR("NBD_OPT_INFO: not implemented");
			break;
		case NBD_OPT_GO:
		{   
			uint16_t flags;
			INFO("Processing NBD_OPT_EXORT_NAME");

			provide_server_info(server, &exportsize, &flags);
	
			if (send_info_export (new_fd, opt_code,
                                                    NBD_REP_INFO,
                                                    NBD_INFO_EXPORT,
                                                    exportsize, flags) == -1){
				ERR("Could not send reply info export");
				ret = -1;	
				goto done;
			}
			
			/* 
			 * We could read the option infos here but nothing we
			 * connect to seems to use them
			 */
			if (send_option_reply (new_fd, NBD_OPT_GO, NBD_REP_ACK) == -1){
				ERR("Could not send new style option reply");
				ret = -1;	
				goto done;
			}
		}
			break;
		case NBD_OPT_STRUCTURED_REPLY:
			/* 
			 * We are always going to do structured replies so just acknowledge the
			 * request
			 */
			INFO("Processing NBD_OPT_STRUCTURED_REPLY");
			if (opt_len != 0) {
				send_option_reply (new_fd, opt_code, NBD_REP_ERR_INVALID);
				ret = -1;	
				goto done;
			} 
	  
			if (send_option_reply (new_fd, opt_code, NBD_REP_ACK) == -1){
				ret = -1;	
				goto done;
			}
	  
			break;
		case NBD_OPT_LIST_META_CONTEXT:
			ERR("NBD_OPT_LIST_META_CONTEXT: not implemented");
			break;
		case NBD_OPT_SET_META_CONTEXT:
		{
			INFO("Processing NBD_OPT_SET_META_CONTEXT");
			uint32_t opt_index;
			uint32_t exportnamelen;
			uint32_t nr_queries;
			uint32_t querylen;
			if (recv(new_fd, buf, opt_len, 0) == -1){
				ret = -1;	
				goto done;
			}
        
			memcpy (&exportnamelen, &buf[0], 4);
			exportnamelen = be32toh (exportnamelen);
			opt_index = 4 + exportnamelen;
	
			memcpy (&nr_queries, &buf[opt_index], 4);
			nr_queries = be32toh (nr_queries);
			opt_index += 4;
			while (nr_queries > 0) {
				char temp[16];
				memcpy (&querylen, &buf[opt_index], 4);
				querylen = be32toh (querylen);
				opt_index += 4;
				memcpy (temp, &buf[opt_index], 15);
				temp[15]= '\0';
				INFO("actual string %s\n", temp);
				if (querylen == 15 && strncmp (&buf[opt_index], "base:allocation", 15) == 0) {
				    if(send_meta_context (new_fd, NBD_REP_META_CONTEXT, 1, "base:allocation") !=0) {
					ret = -1;	
					goto done;
				    }
				}
				nr_queries--;
			}
			if (send_option_reply (new_fd, opt_code, NBD_REP_ACK) == -1){
				ret = -1;	
				goto done;
			}
		}
			break;
		default:
			ret = -1;	
			goto done;
		}

		free(buf);
		buf = NULL;

		/* Loop ends here for these commands */
		if (opt_code == NBD_OPT_GO || opt_code == NBD_OPT_EXPORT_NAME )
			break;
	} 

	if (n_options >= MAX_OPTIONS) {
		ERR("Max number of nbd options exceeded (%d)", MAX_OPTIONS);
		ret = -1;	
		goto done;
	}

done:
	free(buf);
	return ret;
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
		tapdisk_nbdserver_set_free_request(client, &client->reqs[i]);
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

	close(client->client_fd);
	client->client_fd = -1;

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
__tapdisk_nbdserver_block_status_cb(td_vbd_request_t *vreq, int err,
		void *token, int final)
{
	td_nbdserver_client_t *client = token;
	td_nbdserver_req_t *req = container_of(vreq, td_nbdserver_req_t, vreq);
	tapdisk_extents_t* extents = (tapdisk_extents_t *)(vreq->data);
	send_structured_reply_block_status (client->client_fd, req->id, extents);
	free_extents(extents);
	free(vreq->iov->base);
	tapdisk_nbdserver_free_request(client, req);
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

	if (interval > 20 * 1000 * 1000) {
		INFO("request took %llu microseconds to complete", interval);
	}

	if (client->client_fd < 0) {
		ERR("Finishing request for client that has disappeared");
		goto finish;
	}

	tosend = len = sizeof(reply);
	while (tosend > 0) {
		sent = send(client->client_fd,
			    ((char *)&reply) + (len - tosend),
			    tosend, 0);
		if (sent <= 0) {
			sent = errno;
			ERR("Short send/error in callback: %s", strerror(sent));
			goto finish;
		}	
		tosend -= sent;
	}

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

void
tapdisk_nbdserver_handshake_cb(event_id_t id, char mode, void *data)
{
	uint32_t cflags = 0;

	td_nbdserver_t *server = (td_nbdserver_t* )data;

	int rc = recv(server->handshake_fd, &cflags, sizeof(cflags), 0);
	if(rc < sizeof(cflags)) {
		ERR("Could not receive client flags");
		return;
	}

	cflags = be32toh (cflags);
	bool no_zeroes = (NBD_FLAG_NO_ZEROES & cflags) != 0;

        /* Receive newstyle options. */
        if (receive_newstyle_options (server, server->handshake_fd, no_zeroes) == -1){
		ERR("Option negotiation messed up");
	}

	tapdisk_server_unregister_event(id);
}

int
tapdisk_nbdserver_new_protocol_handshake(td_nbdserver_t *server, int new_fd)
{
	struct nbd_new_handshake handshake;

	handshake.nbdmagic = htobe64 (NBD_MAGIC);
	handshake.version = htobe64 (NBD_NEW_VERSION);
	handshake.gflags = htobe16 (gflags);

	int rc = send(new_fd, &handshake, sizeof(handshake), 0);
	if (rc != sizeof(handshake)) {
		ERR("Sending newstyle handshake");
		return -1;
	}
	server->handshake_fd = new_fd;
	/* We may need to wait upto 40 seconds for a reply especially during
	 * SXM contexts, so setup an event and return so that tapdisk is 
	 * reponsive during the interim*/

	tapdisk_server_register_event( SCHEDULER_POLL_READ_FD,
				       new_fd, TV_ZERO,
				       tapdisk_nbdserver_handshake_cb,
				       server);
	return 0;
}

static void
tapdisk_nbdserver_newclient_fd_old(td_nbdserver_t *server, int new_fd)
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
	INFO("Sending size %"PRIu64"", ntohll(tmp64));
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
		return;
	}

	INFO("About to alloc client");
	client = tapdisk_nbdserver_alloc_client(server);
	if (client == NULL) {
		ERR("Error allocating client");
		close(new_fd);
		return;
	}

	INFO("Got an allocated client at %p", client);
	client->client_fd = new_fd;

	INFO("About to enable client on fd %d", client->client_fd);
	if (tapdisk_nbdserver_enable_client(client) < 0) {
		ERR("Error enabling client");
		tapdisk_nbdserver_free_client(client);
		close(new_fd);
	}
}

static void
tapdisk_nbdserver_newclient_fd_new_fixed(td_nbdserver_t *server, int new_fd)
{
	td_nbdserver_client_t *client;

	ASSERT(server);
	ASSERT(new_fd >= 0);

	INFO("Got a new client!");

	if(tapdisk_nbdserver_new_protocol_handshake(server, new_fd) != 0)
		return;

	INFO("About to alloc client");
	client = tapdisk_nbdserver_alloc_client(server);
	if (client == NULL) {
		ERR("Error allocating client");
		close(new_fd);
		return;
	}

	INFO("Got an allocated client at %p", client);
	client->client_fd = new_fd;

	INFO("About to enable client on fd %d", client->client_fd);
	if (tapdisk_nbdserver_enable_client(client) < 0) {
		ERR("Error enabling client");
		tapdisk_nbdserver_free_client(client);
		close(new_fd);
	}
}

static void
tapdisk_nbdserver_newclient_fd(td_nbdserver_t *server, int new_fd)
{
	if(SERVER_USE_OLD_PROTOCOL){
		tapdisk_nbdserver_newclient_fd_old(server, new_fd);
	} else {
		tapdisk_nbdserver_newclient_fd_new_fixed(server, new_fd);
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
		rc = recv(fd, ptr + n, hdrlen - n, 0);
		if (rc == 0) {
			rc = errno;
			if (rc == EAGAIN)
				return;
			goto fail;
		}
		if (rc < 0) {
			rc = errno;
			ERR("failed to receive from client errno: %s. Closing connection",
			    strerror(rc));
			goto fail;
		}
		n += rc;
	}

	if (request.magic != htonl(NBD_REQUEST_MAGIC)) {
		ERR("Not enough magic expected %08x got %08x",
		    NBD_REQUEST_MAGIC, ntohl(request.magic));
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
		INFO("Received close message - closing connection");
		tapdisk_nbdserver_free_client(client);
		return;
	case TAPDISK_NBD_CMD_BLOCK_STATUS:
	{
		tapdisk_extents_t *extents = (tapdisk_extents_t*)malloc(sizeof(tapdisk_extents_t));
		if(extents == NULL) {
			ERR("Could not allocate memory for tapdisk_extents_t");
			goto fail;
		}	
		bzero(extents, sizeof(tapdisk_extents_t));
		vreq->data = extents;
		vreq->cb = __tapdisk_nbdserver_block_status_cb;
		vreq->op = TD_OP_BLOCK_STATUS;
	}
		break;
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

	INFO("Received fd %d with msg: %s", fd, msg);

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
tapdisk_nbdserver_pause(td_nbdserver_t *server, bool log)
{
	struct td_nbdserver_client *pos, *q;

	if (log) {
		INFO("NBD server pause(%p)", server);
	}

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
		if ((server->fdrecv_listening_fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0)) ==
				-1) {
			ERR("Failed to create socket");
			continue;
		}

		if (setsockopt(server->fdrecv_listening_fd, SOL_SOCKET, SO_REUSEADDR,
					&yes, sizeof(int)) == -1) {
			ERR("Failed to setsockopt");
			close(server->fdrecv_listening_fd);
			server->fdrecv_listening_fd = -1;
			continue;
		}

		if (bind(server->fdrecv_listening_fd, p->ai_addr, p->ai_addrlen) ==
				-1) {
			ERR("Failed to bind");
			close(server->fdrecv_listening_fd);
			server->fdrecv_listening_fd = -1;
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
	int new_fd;
	ASSERT(server);
	ASSERT(server->unix_listening_fd == -1);

	new_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (new_fd == -1) {
		err = -errno;
		ERR("failed to create UNIX domain socket: %s\n", strerror(-err));
		goto out_no_fd;
	}

	server->local.sun_family = AF_UNIX;

	if (unlikely(strlen(server->sockpath) > 
		     (sizeof(server->local.sun_path) - 1))) {
		err = -ENAMETOOLONG;
		ERR("socket name too long: %s\n", server->sockpath);
		goto out;
	}

	strcpy(server->local.sun_path, server->sockpath);
	err = unlink(server->local.sun_path);
	if (err == -1 && errno != ENOENT) {
		err = -errno;
		ERR("failed to remove %s: %s\n", server->local.sun_path,
				strerror(-err));
		goto out;
	}
	len = strlen(server->local.sun_path) + sizeof(server->local.sun_family);
	err = bind(new_fd, (struct sockaddr *)&server->local,
			len);
	if (err == -1) {
		err = -errno;
		ERR("failed to bind: %s\n", strerror(-err));
		goto out;
	}

	err = listen(new_fd, 10);
	if (err == -1) {
		err = -errno;
		ERR("failed to listen: %s\n", strerror(-err));
		goto out;
	}

	server->unix_listening_fd = new_fd;
	err = tapdisk_nbdserver_unpause_unix(server);
	if (err) {
		ERR("failed to unpause the NBD server (unix): %s\n",
				strerror(-err));
		goto out;
	}

	INFO("Successfully started NBD server on %s\n", server->sockpath);

out:
	if (err) {
		close(new_fd);
		server->unix_listening_fd = -1;
	}

out_no_fd:

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
			if((err = tapdisk_nbdserver_enable_client(pos)) < 0) {
				ERR("Failed to enable nbd client after pause");
				return err;
			}
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
