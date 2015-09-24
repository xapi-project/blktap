#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "payload.h"

#define PFX_SIZE 5
#define TCP_BACKLOG 10


/**
 * This function parse the given path and populate the other char arrays
 * with volume group and logical volume names (if any).
 * The accepted patter is the following
 * /dev/<name1>/<name2> where <name1> and <name2> can contain any
 * char other than '/' and are interpreted, respectively as VG name
 * and LV name. Any other pattern is wrong (/dev/mapper/<name> is accepted
 * but it is not what you want..).
 * 
 * @param[in] path is a NULL terminated char array containing the path to parse
 * @param[out] vg if successful contains VG name. Caller must ensure allocated
 * space is bigger than #path
 * @param[out] lv if successful contains LV name. Caller must ensure allocated
 * space is bigger than #path
 * @return 0 if OK and 1 otherwise. On exit all the array are NULL terminated
 */
int
kpr_split_lvm_path(const char * path, char * vg, char * lv)
{
	static const char dev_pfx[PFX_SIZE] = "/dev/"; /* no \0 */
	bool flag = false;

	if( strncmp(dev_pfx, path, PFX_SIZE) ) {
		fprintf(stderr, "Not a device pattern\n");
		return 1;
	}
	path += PFX_SIZE;

	/* Extract VG */
	for( ; *path; ++path, ++vg ) {
		if( *path == '/' ) {
			*vg = '\0';
			break;
		}
		*vg = *path;
		flag = true;
	}

	/* Check why and how the loop ended */
	if ( *path == '\0' || !flag ) {
		/* terminate strings and error */
		*vg = '\0';
		*lv = '\0';
		return 1;
	}

	/* Extract LV */
	++path; /* skip slash */
	for( flag = false; *path; ++path, ++lv ) {
		if( *path == '/' ) {
			fprintf(stderr, "too many slashes\n");
			*lv = '\0';
			return 1;
		}
		*lv = *path;
		flag = true;
	}
	*lv = '\0'; /* terminate string */

	return flag ? 0 : 1;
}


/**
 * Create, bind and listen to specified socket
 *
 * @param[in] port number
 * @return file descriptor of socket or -1
 */
int
kpr_tcp_create(uint16_t port)
{
	int sfd;
	struct sockaddr_in s_addr;

	/* create tcp socket */
	sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd == -1) {
		fprintf(stderr, "tcp socket error");
		return -1;
	}

	/* Build socket address, bind and listen */
	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_family = AF_INET;
	s_addr.sin_port = htons(port);
	s_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sfd, (struct sockaddr *) &s_addr, sizeof(s_addr)) == -1) {
		fprintf(stderr, "bind error");
		goto fail;
	}

	if (listen(sfd, TCP_BACKLOG) == -1) {
		fprintf(stderr, "listen error");
		goto fail;
	}

	return sfd;
fail:
	close(sfd);
	return -1;
}


int
kpr_tcp_conn_tx_rx(const char *ip, uint16_t port, struct payload * message)
{
	int sfd, ret, len;
	struct sockaddr_in s_addr;
	struct in_addr ipaddr;

	if ( !inet_aton(ip, &ipaddr) ) {
		ret = 1;
		goto end;
	}

	len = sizeof(struct payload);

	/* create tcp socket */
	sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd == -1) {
		ret = 1;
		goto end;
	}

	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_family = AF_INET;
	s_addr.sin_port = htons(port);
	s_addr.sin_addr = ipaddr;

	if ( connect(sfd, (struct sockaddr *) &s_addr, sizeof(s_addr)) ) {
		ret = 1;
		goto end;
	}

	/* TBD: very basic write, need a while loop */
	if (write(sfd, message, len) != len) {
		ret = 1;
		goto end;
	}

	/* TBD: very basic read */
	if (read(sfd, message, len) != len) {
		ret = 2;
		goto end;
	}

end:
	close(sfd);
	return 0;    /* Closes our socket; server sees EOF */

}
