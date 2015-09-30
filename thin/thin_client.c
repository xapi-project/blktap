#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "debug.h"
#include "../drivers/tapdisk-log.h"
#include "blktap.h"
#include "payload.h"

struct thin_conn_handle {
	int sfd;
};

int thin_sync_send_and_receive(struct thin_conn_handle *ch,
		struct payload *message)
{
	size_t len = sizeof(struct payload);
	int ret;

	if (ch == NULL) {
		return -1;
	}

	/* Send messages to server */
write:
	ret = write(ch->sfd, message, len);
	if (ret == -1 && errno == EINTR)
		goto write;
	if (ret != len)
		return -errno;

	/* Wait for ACK packet */
read:
	ret = read(ch->sfd, message, len);
	if (ret == -1 && errno == EINTR)
		goto read;
	if (ret != len)
		return -errno;

	return 0;
}

struct thin_conn_handle *
thin_connection_create(void)
{
	struct sockaddr_un svaddr, claddr;
	struct thin_conn_handle *ch;
	char client_sock_name[64];
	struct timeval timeout;
	int ret;

	ch = malloc(sizeof(struct thin_conn_handle));
	if (ch == NULL)
		goto out2;

	ch->sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (ch->sfd == -1) {
		EPRINTF("Socket creation failed");
		goto out1;
	}

	timeout.tv_sec = 10;
	timeout.tv_usec = 0;
	ret = setsockopt(ch->sfd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			 sizeof(struct timeval));
	if (ret < 0) {
		EPRINTF("Socket set timeout failed");
		goto out1;
	}

	sprintf(client_sock_name, "td_thin_client_%d", getpid());

	/* Construct address of the client*/
	memset(&claddr, 0, sizeof(struct sockaddr_un));
	claddr.sun_family = AF_UNIX;
	strncpy(&claddr.sun_path[1], client_sock_name , strlen(client_sock_name)); 

	if (bind(ch->sfd, (struct sockaddr *) &claddr, 
		sizeof(sa_family_t) + strlen(client_sock_name) + 1) == -1) {
		EPRINTF("Bind has failed");
		goto out3;
	}

	/* Construct address of server */
	memset(&svaddr, 0, sizeof(struct sockaddr_un));
	svaddr.sun_family = AF_UNIX;
	strncpy(svaddr.sun_path, THIN_CONTROL_SOCKET, sizeof(svaddr.sun_path) - 1);

	/* Connect to the server */
	if (connect(ch->sfd, (struct sockaddr *) &svaddr, sizeof(svaddr)) == -1) {
		EPRINTF("Connect has failed");
		goto out3;
	}

	/* All went well, just return the opaque structure */
	return ch;

out3:
	close(ch->sfd);
out1:
	free(ch);
out2:
	return NULL;
}

void
thin_connection_destroy(struct thin_conn_handle *ch)
{
	if (ch == NULL) {
		EPRINTF("Was asked to destroy a NULL handle");
		return;
	}
	/* WARING:
	 * Close could fail, but not sure what to do if that happes
	 */
	close(ch->sfd);
	free(ch);
}
