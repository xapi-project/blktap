/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#ifndef __BLOCK_AIO_H__
#define __BLOCK_AIO_H__

#include "tapdisk.h"
#include "tapdisk-queue.h"


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
