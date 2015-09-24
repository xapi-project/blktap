#ifndef _PAYLOAD_H
#define _PAYLOAD_H    1
#include <sys/types.h>
#include <inttypes.h>
#include "tapdisk-message.h"

#define PAYLOAD_MAX_PATH_LENGTH TAPDISK_MESSAGE_MAX_PATH_LENGTH
#define IP_MAX_LEN 32

#define THIN_ERR_CODE_SUCCESS 0
#define THIN_ERR_CODE_FAILURE 1

#define PAYLOAD_CB_NONE 0
#define PAYLOAD_CB_SOCK 1

typedef enum {
	PAYLOAD_RESIZE = 0,
	PAYLOAD_CLI,
	PAYLOAD_STATUS,
	PAYLOAD_UNDEF
} payload_message_t;

struct payload {
	uint8_t type;
	char path[PAYLOAD_MAX_PATH_LENGTH];
	uint64_t req_size;
	uint8_t cb_type;
	char cb_data[128];
	uint16_t err_code;
	uint32_t reserved_clt;
	uint32_t reserved_srv;
	char id[128];
};

struct thin_conn_handle;
int thin_sync_send_and_receive(struct thin_conn_handle *ch,
		struct payload *message);
struct thin_conn_handle * thin_connection_create(void);
void thin_connection_destroy(struct thin_conn_handle *ch);
int init_payload(struct payload *);
void print_payload(struct payload *);

#endif /* payload.h */
