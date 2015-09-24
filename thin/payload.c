#include <stdio.h>
#include "payload.h"

int init_payload(struct payload *pload)
{
	pload->id = -1;
	pload->curr = 0;
	pload->req = 0;
	pload->vhd_size = 0;
	pload->reply = PAYLOAD_UNDEF;
	pload->ipaddr[0] = '\0';
	return 0;
}

void print_payload(struct payload *pload)
{
	printf("payload data:\n");
	printf("id = %d\n", pload->id);
	printf("path = %s\n", pload->path);
	printf("current size = %"PRIu64"\n", pload->curr);
	printf("requested size = %"PRIu64"\n", pload->req);
	printf("virtual size = %"PRIu64"\n", pload->vhd_size);
	printf("request type = %d\n", pload->reply);
	printf("dest ipaddr = %s\n", pload->ipaddr);
	return;
}
