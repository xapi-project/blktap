#include <string.h>
#include "payload.h"
#include "thin_log.h"

int init_payload(struct payload *pload)
{
	memset(pload, 0, sizeof(struct payload));
	return 0;
}

void print_payload(struct payload *pload)
{
	thin_log_info("payload data:\n");
	thin_log_info("type = %d\n", pload->type);
	thin_log_info("path = %s\n", pload->path);
	thin_log_info("requested size = %"PRIu64"\n", pload->req_size);
	thin_log_info("cb_type = %d\n", pload->cb_type);
	thin_log_info("err_code = %d\n", pload->err_code);
	return;
}
