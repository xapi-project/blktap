#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "blktap.h"
#include "payload.h"

int
thin_sock_comm(struct payload *message)
{
	/* maybe we can borrow tap_ctl_connect but it is risky because
	   of strcpy */

	struct sockaddr_un addr;
	int sfd, len;
	pid_t pid;

	len = sizeof(struct payload);

	pid = getpid();
	message->id = pid;

	sfd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
	if (sfd == -1)
		return -errno; /* to be handled */

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, THIN_CONTROL_SOCKET, sizeof(addr.sun_path) - 1);

	if (connect(sfd, (struct sockaddr *) &addr, sizeof(addr)) == -1)
		return -errno;

	/* TBD: very basic write, need a while loop */
	if (write(sfd, message, len) != len)
		return -errno;

	/* TBD: very basic read */
	if (read(sfd, message, len) != len)
		return -errno;

	close(sfd);
	return 0;    /* Closes our socket; server sees EOF */
}

