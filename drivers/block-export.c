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

#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "export: " _f, ##_a)
#define ERROR(_f, _a...)           tlog_syslog(TLOG_WARN, "export: " _f, ##_a)

struct vdi_chunk_format {
	uint64_t offset;
	uint32_t length;
	char payload[0];
} __attribute__((__packed__));

struct vdi_chunk {
	unsigned long bytes_to_send;
	unsigned long bytes_sent;
	int send_terminator;
	struct vdi_chunk_format chunk;
};

struct tdexport_data {
	/* Socket details */
	int                 socket;
	struct sockaddr_in *remote;
	char               *peer_ip;

	/* HTTP import protocol details */
	char               *session_ref;
	char               *vdi_ref;
};

int tdexport_connect_import_session(struct tdexport_data *prv)
{
	int sock;
	int opt = 1;
	int rc;
	char *msg;

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
	prv->remote->sin_port = htons(80);
  
	if (connect(sock, (struct sockaddr *)prv->remote, sizeof(struct sockaddr)) < 0) {
		ERROR("Could not connect to peer: %s\n", strerror(errno));
		close(sock);
		return -1;
	}

#define IMPORT_URL_FMT "PUT /import_raw_vdi?session_id=%s&vdi=%s&chunked=true&o_direct=true HTTP/1.0\r\n\r\n"
	msg = malloc(strlen(IMPORT_URL_FMT) +
                     strlen(prv->session_ref) +
                     strlen(prv->vdi_ref) +1 );
	if (!msg) {
		ERROR("HTTP PUT message malloc failure\n");
		close(sock);
		return -1;
	}
	sprintf(msg, IMPORT_URL_FMT, prv->session_ref, prv->vdi_ref);
	INFO("Using query: %s\n", msg);

	rc = send(sock, msg, strlen(msg), 0);
	if (rc == -1) {
		ERROR("Error sending PUT request: %s\n", strerror(errno));
		close(sock);
		free(msg);
		return -1;
	}
	free(msg);

	/* Wait for HTTP/1.1 200 OK and the double \r\n */
#define RECV_BUFFER_SIZE 4096
	msg = malloc(RECV_BUFFER_SIZE);
	if (!msg) {
		ERROR("Receive buffer malloc failure\n");
		close(sock);
		return -1;
	}
	for (;;) {
		rc = recv(sock, msg, RECV_BUFFER_SIZE, 0);
		if ( rc < 0) {
			ERROR("Error receiving from peer: %s", strerror(errno));
			free(msg);
			close(sock);
			return -1;
		}
		if (strstr(msg, "HTTP/1.1 200 OK")) {
			INFO("Received from peer: HTTP/1.1 200 OK\n");
		}
		if (strstr(msg, "\r\n\r\n")) {
			break;
		}
	}
	free(msg);

	prv->socket = sock;
	return sock;
}

int tdexport_send_chunk(struct tdexport_data *prv, const char *buffer, uint32_t length, uint64_t offset)
{
	struct vdi_chunk_format chunk;
	ssize_t rc;
	uint32_t bytes_to_send;
	uint32_t bytes_sent;

	INFO("Sending chunk of size %u at offset %"PRIu64"\n", length, offset);
	if (prv->socket == 0) {
		ERROR("Cannot send chunk without an open socket\n");
		return -1;
	}

	chunk.length = length;
	chunk.offset = offset;
	rc = send(prv->socket, &chunk, sizeof(chunk), 0);
	if (rc < 0) {
		ERROR("Error sending chunk header: %s\n", strerror(errno));
		return -1;
	}
	if (rc != sizeof(chunk)) {
		ERROR("Send of %d bytes of chunk header does not match expected %d\n", (int)rc, (int)sizeof(chunk));
	}

	bytes_to_send = length;
	bytes_sent = 0;

	while(bytes_to_send) {
		rc = send(prv->socket, buffer + bytes_sent, bytes_to_send, 0);
		if (rc < 0) {
			ERROR("Error sending chunk payload: %s\n", strerror(errno));
			return -1;
		}
		if ((uint32_t)rc > bytes_to_send)
			rc = bytes_to_send;
		bytes_sent += (uint32_t)rc;
		bytes_to_send -= (uint32_t)rc;
	}

	return 0;
}

int tdexport_recv_result(struct tdexport_data *prv)
{
	uint32_t result;
	ssize_t rc;

	if (prv->socket == 0) {
		ERROR("Cannot receive result an open socket\n");
		return -1;
	}
	rc = recv(prv->socket, &result, sizeof(result), 0);
	if (rc < 0) {
		ERROR("Error receiving result: %s\n", strerror(errno));
		return -1;
	}
	if (result != 0) {
		ERROR("Received non-zero result: %d (mirror is out-of-sync)", result);
		return -1;
	}

	INFO("Received successful result; mirror is synchronised OK\n");

	return 0;
}

int tdexport_send_terminator_chunk(struct tdexport_data *prv)
{
	struct vdi_chunk_format zero;	
	ssize_t rc;

	if (prv->socket == 0) {
		ERROR("Cannot send terminator chunk without an open socket\n");
		return -1;
	}

	zero.offset = 0ULL;
	zero.length = 0UL;

	rc = send(prv->socket, &zero, sizeof(zero), 0);
	if (rc < 0) {
		ERROR("Error sending chunk terminator: %s\n", strerror(errno));
		return -1;
	}
	if (rc != sizeof(zero)) {
		ERROR("Send of %d bytes of terminator does not match expected %u\n", (int)rc, (int)sizeof(zero));
		return -1;
	}
	INFO("Terminator chunk written successfully\n");
	return 0;
}

/* -- interface -- */

static int tdexport_close(td_driver_t*);

static int tdexport_open(td_driver_t* driver, const char* name, td_flag_t flags)
{
	struct tdexport_data *prv;
	char peer_ip[256];
	char session_ref[256];
	char vdi_ref[256];
	int rc;

	driver->info.sector_size = 512;
	driver->info.info = 0;

	prv = (struct tdexport_data *)driver->data;
	memset(prv, 0, sizeof(struct tdexport_data));

	INFO("Opening export to %s\n", name);

	rc = sscanf(name, "%255[^/]/%255[^/]/%255[^\n]", peer_ip, session_ref, vdi_ref);
	if (rc != 3) {
		ERROR("Could not parse export URL");
		return -1;
	}
	prv->peer_ip = malloc(strlen(peer_ip) + 1);
	prv->session_ref = malloc(strlen(session_ref) + 1);
	prv->vdi_ref = malloc(strlen(vdi_ref) + 1);
	if (!prv->peer_ip || !prv->session_ref || !prv->vdi_ref) {
		ERROR("Failure to malloc for URL parts");
		return -1;
	}
	strcpy(prv->peer_ip, peer_ip);
	strcpy(prv->session_ref, session_ref);
	strcpy(prv->vdi_ref, vdi_ref);

	INFO("Export peer=%s session=%s VDI=%s\n", prv->peer_ip, prv->session_ref, prv->vdi_ref);
	if (tdexport_connect_import_session(prv) < 0) {
		return -1;
	}

	return 0;
}

static int tdexport_close(td_driver_t* driver)
{
	struct tdexport_data *prv;

	prv = (struct tdexport_data *)driver->data;
	if (prv->socket) {
		tdexport_send_terminator_chunk(prv);
		tdexport_recv_result(prv);
		close(prv->socket);
		prv->socket = 0;
	}

	if (prv->peer_ip) {
		free(prv->peer_ip);
		prv->peer_ip = NULL;
	}
	if (prv->session_ref) {
		free(prv->session_ref);
		prv->session_ref = NULL;
	}
	if (prv->vdi_ref) {
		free(prv->vdi_ref);
		prv->vdi_ref = NULL;
	}

	return 0;
}

static void tdexport_queue_read(td_driver_t* driver, td_request_t treq)
{
        int      size    = treq.secs * driver->info.sector_size;
        uint64_t offset  = treq.sec * (uint64_t)driver->info.sector_size;
        INFO("READ %"PRIu64" (%d)\n", offset, size);
	td_forward_request(treq);
	//td_complete_request(treq, 0);
}

static void tdexport_queue_write(td_driver_t* driver, td_request_t treq)
{
	struct tdexport_data *prv = (struct tdexport_data *)driver->data;
	int      size    = treq.secs * driver->info.sector_size;
	uint64_t offset  = treq.sec * (uint64_t)driver->info.sector_size;
        
	//memcpy(img + offset, treq.buf, size);

	INFO("WRITE 0x%"PRIu64" (%u)\n", offset, size);

	tdexport_send_chunk(prv, treq.buf, size, offset);

	td_complete_request(treq, 0);
}

static int tdexport_get_parent_id(td_driver_t* driver, td_disk_id_t* id)
{
	return -EINVAL;
}

static int tdexport_validate_parent(td_driver_t *driver,
				    td_driver_t *parent, td_flag_t flags)
{
	return 0;
}

struct tap_disk tapdisk_export = {
  .disk_type          = "tapdisk_export",
  .private_data_size  = sizeof(struct tdexport_data),
  .flags              = 0,
  .td_open            = tdexport_open,
  .td_close           = tdexport_close,
  .td_queue_read      = tdexport_queue_read,
  .td_queue_write     = tdexport_queue_write,
  .td_get_parent_id   = tdexport_get_parent_id,
  .td_validate_parent = tdexport_validate_parent,
};
