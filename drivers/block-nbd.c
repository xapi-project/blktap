#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include "tapdisk.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-utils.h"
#include "tapdisk-fdreceiver.h"
#include "nbd.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "nbd: " _f, ##_a)
#define ERROR(_f, _a...)           tlog_syslog(TLOG_WARN, "nbd: " _f, ##_a)

#define N_PASSED_FDS 10
#define TAPDISK_NBDCLIENT_MAX_PATH_LEN 256
#define TAPDISK_NBDCLIENT_LISTENING_SOCK_PATH "/var/run/blktap-control/nbdclient"
/* We'll only ever have one nbdclient fd receiver per tapdisk process, 
   so let's just store it here globally. We'll also keep track of the 
   passed fds here too. */

struct td_fdreceiver *fdreceiver=NULL;

struct tdnbd_passed_fd {
	char id[40];
	struct timeval t;
	int fd;
} passed_fds[N_PASSED_FDS];




struct tdnbd_data {
	/* Socket details */
	int                 socket;
	struct sockaddr_in *remote;
	char               *peer_ip;
    int                 port;
};

/* -- fdreceiver bits and pieces -- */

void
tdnbd_stash_passed_fd(int fd, char *msg, void *data) 
{
	int free_index=-1;
	int i;
	for(i=0; i<N_PASSED_FDS; i++) {
		if(passed_fds[i].fd == -1) {
			free_index=i;
			break;
		}
	}

	if(free_index==-1) {
		ERROR("Error - more than %d fds passed! cannot stash another.",N_PASSED_FDS);
		close(fd);
		return;
	}

	passed_fds[free_index].fd=fd;
	strncpy(passed_fds[free_index].id, msg, sizeof(passed_fds[free_index].id));
	gettimeofday(&passed_fds[free_index].t, NULL);

}

int tdnbd_retreive_passed_fd(const char *name) 
{
	int fd, i;

	for(i=0; i<N_PASSED_FDS; i++) {
		if(strncmp(name, passed_fds[i].id,sizeof(passed_fds[i].id))==0) {
			fd=passed_fds[i].fd;
			passed_fds[i].fd = -1;
			return fd;
		}
	}

	ERROR("Couldn't find the fd named: %s",name);

	return -1;
}

void 
tdnbd_fdreceiver_start() 
{
	char fdreceiver_path[TAPDISK_NBDCLIENT_MAX_PATH_LEN];
	int i;

	/* initialise the passed fds list */
	for(i=0; i<N_PASSED_FDS; i++) {
		passed_fds[i].fd = -1;
	}

	snprintf(fdreceiver_path, TAPDISK_NBDCLIENT_MAX_PATH_LEN,
			 "%s%d", TAPDISK_NBDCLIENT_LISTENING_SOCK_PATH, getpid());

	fdreceiver=td_fdreceiver_start(fdreceiver_path,
								   tdnbd_stash_passed_fd, NULL);

}

int tdnbd_nbd_negotiate(struct tdnbd_data *prv, td_driver_t *driver)
{
#define RECV_BUFFER_SIZE 256
	int rc;
	char buffer[RECV_BUFFER_SIZE];
	uint64_t magic;
	uint64_t size;
	uint32_t flags;
	int padbytes = 124;
	int sock = prv->socket;

	/* NBD negotiation protocol: 
	 *
	 * Server sends 'NBDMAGIC'
	 * then it sends 0x00420281861253L
	 * then it sends a 64 bit bigendian size
	 * then it sends a 32 bit bigendian flags
	 * then it sends 124 bytes of nothing
	 *
	 */

	rc = recv(sock, buffer, 8, 0);
	if(rc<8) {
	  ERROR("Short read in negotiation(1) (%d)\n",rc);
	  close(sock);
	  return -1;
	} 

	if(memcmp(buffer, "NBDMAGIC", 8) != 0) {
	  buffer[8]=0;
	  ERROR("Error in NBD negotiation: got '%s'",buffer);
	  close(sock);
	  return -1;
	}

	rc = recv(sock, &magic, sizeof(magic), 0);
	if(rc<8) {
	  ERROR("Short read in negotiation(2) (%d)\n",rc);
	  close(sock);
	  return -1;
	} 

	if(ntohll(magic) != NBD_NEGOTIATION_MAGIC) {
	  ERROR("Not enough magic in negotiation(2) (%"PRIu64")\n",ntohll(magic));
	  close(sock);
	  return -1;
	}

	rc = recv(sock, &size, sizeof(size), 0);
	if(rc<sizeof(size)) {
	  ERROR("Short read in negotiation(3) (%d)\n",rc);
	  close(sock);
	  return -1;
	} 
	
	INFO("Got size: %"PRIu64"", ntohll(size));

	driver->info.size = ntohll(size) >> SECTOR_SHIFT;
	driver->info.sector_size = DEFAULT_SECTOR_SIZE;
	driver->info.info = 0;

	rc = recv(sock, &flags, sizeof(flags), 0);
	if(rc<sizeof(flags)) {
	  ERROR("Short read in negotiation(4) (%d)\n",rc);
	  close(sock);
	  return -1;
	} 

	INFO("Got flags: %"PRIu32"", ntohl(flags));

	while(padbytes>0) {
	  rc = recv(sock, buffer, padbytes, 0);
	  if(rc<0) {
		ERROR("Bad read in negotiation(5) (%d)\n",rc);
		close(sock);
		return -1;
	  }
	  padbytes -= rc;
	}
	
	INFO("Successfully connected to NBD server");

	return 0;
}


int tdnbd_connect_import_session(struct tdnbd_data *prv, td_driver_t* driver)
{
	int sock;
	int opt = 1;
	int rc;

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		ERROR("Could not create socket: %s\n", strerror(errno));
		return -1;
	}

	rc = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt));
	if (rc < 0) {
		ERROR("Could not set TCP_NODELAY: %s\n", strerror(errno));
		return -1;
	}

	prv->remote = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in *));
	if (!prv->remote) {
		ERROR("struct sockaddr_in malloc failure\n");
		close(sock);
		return -1;
	}
	prv->remote->sin_family = AF_INET;
	rc = inet_pton(AF_INET, prv->peer_ip, &(prv->remote->sin_addr.s_addr));
	if (rc < 0) {
		ERROR("Could not create inaddr: %s\n", strerror(errno));
		free(prv->remote);
		prv->remote = NULL;
		close(sock);
		return -1;
	}
	else if (rc == 0) {
		ERROR("inet_pton parse error\n");
		free(prv->remote);
		prv->remote = NULL;
		close(sock);
		return -1;
	}
	prv->remote->sin_port = htons(prv->port);
  
	if (connect(sock, (struct sockaddr *)prv->remote, sizeof(struct sockaddr)) < 0) {
		ERROR("Could not connect to peer: %s\n", strerror(errno));
		close(sock);
		return -1;
	}

	prv->socket = sock;

	return tdnbd_nbd_negotiate(prv, driver);
}


int tdnbd_send_chunk(struct tdnbd_data *prv, const char *buffer, uint32_t length, uint64_t offset)
{
	ssize_t rc;
	uint32_t bytes_to_send;
	uint32_t bytes_sent;

	struct nbd_request request;
	struct nbd_reply reply;

	INFO("Sending chunk of size %u at offset %"PRIu64"\n", length, offset);
	if (prv->socket == 0) {
		ERROR("Cannot send chunk without an open socket\n");
		return -1;
	}

	request.magic = htonl(NBD_REQUEST_MAGIC);
	request.type = htonl(NBD_CMD_WRITE);
	request.from = htonll(offset);
	memcpy(request.handle,"tapdiskw",8);
	request.len = htonl(length);
		
	rc = send(prv->socket, &request, sizeof(request), 0);
	if (rc < 0) {
		ERROR("Error sending request header: %s\n", strerror(errno));
		return -1;
	}
	if (rc != sizeof(request)) {
	  ERROR("Send of %d bytes of chunk header does not match expected %u\n", (int)rc, (int)sizeof(request));
	}

	bytes_to_send = length;
	bytes_sent = 0;

	while(bytes_to_send) {
		rc = send(prv->socket, buffer + bytes_sent, bytes_to_send, 0);
		if (rc < 0) {
			ERROR("Error sending request payload: %s\n", strerror(errno));
			return -1;
		}
		if ((uint32_t)rc > bytes_to_send)
			rc = bytes_to_send;
		bytes_sent += (uint32_t)rc;
		bytes_to_send -= (uint32_t)rc;
	}

	rc = recv(prv->socket, &reply, sizeof(reply), 0);
	if(rc < 0) {
	  ERROR("Error receiving response from nbd server: %s\n", strerror(errno));
	  return -1;
	}

	if(ntohl(reply.magic) != NBD_REPLY_MAGIC) {
	  ERROR("Not enough magic in reply from nbd server (got %x)", ntohl(reply.magic));
	  return -1;
	}

	if(memcmp(reply.handle, "tapdiskw", 8) != 0) {
	  ERROR("Handle in reply did not match request (got '%s')",reply.handle);
	  return -1;
	}

	if(ntohl(reply.error) != 0) {
	  ERROR("Got error from nbd server: %d",ntohl(reply.error));
	  return -1;
	}

	return 0;
}

int tdnbd_recv_chunk(struct tdnbd_data *prv, char *buffer, uint32_t length, uint64_t offset)
{
	ssize_t rc;
	uint32_t bytes_to_recv;
	uint32_t bytes_recvd;

	struct nbd_request request;
	struct nbd_reply reply;

	INFO("Receiving chunk of size %u at offset %"PRIu64"\n", length, offset);
	if (prv->socket == 0) {
		ERROR("Cannot send chunk without an open socket\n");
		return -1;
	}

	request.magic = htonl(NBD_REQUEST_MAGIC);
	request.type = htonl(NBD_CMD_READ);
	request.from = htonll(offset);
	memcpy(request.handle,"tapdiskr",8);
	request.len = htonl(length);
		
	rc = send(prv->socket, &request, sizeof(request), 0);
	if (rc < 0) {
		ERROR("Error sending request header: %s\n", strerror(errno));
		return -1;
	}
	if (rc != sizeof(request)) {
	  ERROR("Send of %d bytes of chunk header does not match expected %u\n", (int)rc, (int)sizeof(request));
	}

	rc = recv(prv->socket, &reply, sizeof(reply), 0);
	if(rc < 0) {
	  ERROR("Error receiving response from nbd server: %s\n", strerror(errno));
	  return -1;
	}

	if(ntohl(reply.magic) != NBD_REPLY_MAGIC) {
	  ERROR("Not enough magic in reply from nbd server (got %x)", ntohl(reply.magic));
	  return -1;
	}

	if(memcmp(reply.handle, "tapdiskr", 8) != 0) {
	  ERROR("Handle in reply did not match request (got '%s')",reply.handle);
	  return -1;
	}

	if(ntohl(reply.error) != 0) {
	  ERROR("Got error from nbd server: %d",ntohl(reply.error));
	  return -1;
	}

	bytes_to_recv = length;
	bytes_recvd = 0;

	while(bytes_to_recv) {
		rc = recv(prv->socket, buffer + bytes_recvd, bytes_to_recv, 0);
		if (rc < 0) {
			ERROR("Error receiving reply payload: %s\n", strerror(errno));
			return -1;
		}
		if ((uint32_t)rc > bytes_to_recv)
			rc = bytes_to_recv;
		bytes_recvd += (uint32_t)rc;
		bytes_to_recv -= (uint32_t)rc;
	}

	return 0;
}


/* -- interface -- */

static int tdnbd_close(td_driver_t*);

static int tdnbd_open(td_driver_t* driver, const char* name, td_flag_t flags)
{
	struct tdnbd_data *prv;
	char peer_ip[256];
	int port;
	int rc;

	driver->info.sector_size = 512;
	driver->info.info = 0;

	prv = (struct tdnbd_data *)driver->data;
	memset(prv, 0, sizeof(struct tdnbd_data));

	INFO("Opening nbd export to %s\n", name);

	rc = sscanf(name, "%255[^:]:%d", peer_ip, &port);
	if (rc == 2) {
		prv->peer_ip = malloc(strlen(peer_ip) + 1);
		if (!prv->peer_ip) {
			ERROR("Failure to malloc for NBD destination");
			return -1;
		}
		strcpy(prv->peer_ip, peer_ip);
		prv->port=port;
		
		INFO("Export peer=%s port=%d\n", prv->peer_ip, prv->port);
		if (tdnbd_connect_import_session(prv, driver) < 0) {
			return -1;
		}
		
		return 0;
	} else {
		prv->socket = tdnbd_retreive_passed_fd(name);
		if(prv->socket < 0) {
			ERROR("Couln't find fd named: %s",name);
			return -1;
		}
		INFO("Found passed fd. Connecting...");
		prv->remote = NULL;
		prv->peer_ip = NULL;
		prv->port = -1;
		return tdnbd_nbd_negotiate(prv, driver);
	}
}

static int tdnbd_close(td_driver_t* driver)
{
	struct tdnbd_data *prv;

	prv = (struct tdnbd_data *)driver->data;
	if (prv->socket) {
		close(prv->socket);
	}

	if (prv->peer_ip) {
		free(prv->peer_ip);
		prv->peer_ip = NULL;
	}

	return 0;
}

static void tdnbd_queue_read(td_driver_t* driver, td_request_t treq)
{
  struct tdnbd_data *prv = (struct tdnbd_data *)driver->data;
        int      size    = treq.secs * driver->info.sector_size;
        uint64_t offset  = treq.sec * (uint64_t)driver->info.sector_size;
        INFO("READ %"PRIu64" (%u)\n", offset, size);

		tdnbd_recv_chunk(prv, treq.buf, size, offset);

		td_complete_request(treq, 0);
}

static void tdnbd_queue_write(td_driver_t* driver, td_request_t treq)
{
	struct tdnbd_data *prv = (struct tdnbd_data *)driver->data;
	int      size    = treq.secs * driver->info.sector_size;
	uint64_t offset  = treq.sec * (uint64_t)driver->info.sector_size;
        
	//memcpy(img + offset, treq.buf, size);

	INFO("WRITE %"PRIu64" (%u)\n", offset, size);

	tdnbd_send_chunk(prv, treq.buf, size, offset);

	td_complete_request(treq, 0);
}

static int tdnbd_get_parent_id(td_driver_t* driver, td_disk_id_t* id)
{
	return TD_NO_PARENT;
}

static int tdnbd_validate_parent(td_driver_t *driver,
				    td_driver_t *parent, td_flag_t flags)
{
	return -EINVAL;
}

struct tap_disk tapdisk_nbd = {
  .disk_type          = "tapdisk_nbd",
  .private_data_size  = sizeof(struct tdnbd_data),
  .flags              = 0,
  .td_open            = tdnbd_open,
  .td_close           = tdnbd_close,
  .td_queue_read      = tdnbd_queue_read,
  .td_queue_write     = tdnbd_queue_write,
  .td_get_parent_id   = tdnbd_get_parent_id,
  .td_validate_parent = tdnbd_validate_parent,
};
