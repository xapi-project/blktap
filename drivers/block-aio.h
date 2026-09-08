/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __BLOCK_AIO_H__
#define __BLOCK_AIO_H__

#include "tapdisk.h"


#define MAX_AIO_REQS         TAPDISK_DATA_REQUESTS

struct tdaio_state;

struct aio_request {
	td_request_t         treq;
	struct tiocb         tiocb;
	struct tdaio_state  *state;
};

struct tdaio_state {
	int                  fd;
	td_driver_t         *driver;

	int                  aio_free_count;
	struct aio_request   aio_requests[MAX_AIO_REQS];
	struct aio_request  *aio_free_list[MAX_AIO_REQS];
};

void tdaio_complete(void *arg, struct tiocb *tiocb, int err);

#endif
