#ifndef _PAYLOAD_H
#define _PAYLOAD_H    1
#include <sys/types.h>
#include <inttypes.h>
#include "tapdisk-message.h"

#define PAYLOAD_MAX_PATH_LENGTH TAPDISK_MESSAGE_MAX_PATH_LENGTH
#define IP_MAX_LEN 32

typedef enum {
	/* server */
	PAYLOAD_ACCEPTED = 0,
	PAYLOAD_REJECTED,
	PAYLOAD_DONE,
	PAYLOAD_WAIT,
	/* client */
	PAYLOAD_QUERY,
	PAYLOAD_REQUEST,
	PAYLOAD_CLI,
	/* generic */
	PAYLOAD_UNDEF
} payload_message_t;

struct payload {
	pid_t id;
	char path[PAYLOAD_MAX_PATH_LENGTH];
	uint64_t curr;
	uint64_t req;
	off64_t vhd_size;
	payload_message_t reply;
	char ipaddr[IP_MAX_LEN]; /* used internally */
};

int init_payload(struct payload *);
void print_payload(struct payload *);

/* Temporary location */
int thin_sock_comm(struct payload *);

#endif /* payload.h */
