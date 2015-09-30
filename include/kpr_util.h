#ifndef _KPR_UTIL_H
#define _KPR_UTIL_H

#include "payload.h"

int kpr_split_lvm_path(const char *, char *, char *);
int kpr_tcp_create(uint16_t port);
int kpr_tcp_conn_tx_rx(const char *ip, uint16_t port, struct payload *);

#endif /* KPR_UTIL_H */
