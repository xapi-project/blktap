#include <stdio.h>
#include <string.h>
#include "payload.h"

int init_payload(struct payload *pload)
{
	memset(pload, 0, sizeof(struct payload));
	return 0;
}

void print_payload(struct payload *pload)
{
	printf("payload data:\n");
	printf("type = %d\n", pload->type);
	printf("path = %s\n", pload->path);
	printf("requested size = %"PRIu64"\n", pload->req_size);
	printf("cb_type = %d\n", pload->cb_type);
	printf("err_code = %d\n", pload->err_code);
	return;
}
