/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TAPDISK_INTERFACE_H_
#define _TAPDISK_INTERFACE_H_

#include "tapdisk.h"
#include "io-backend.h"
#include "tapdisk-image.h"
#include "tapdisk-driver.h"

int td_open(td_image_t *, struct td_vbd_encryption *);
int __td_open(td_image_t *, struct td_vbd_encryption *, td_disk_info_t *);
int td_load(td_image_t *);
int td_close(td_image_t *);
int td_get_parent_id(td_image_t *, td_disk_id_t *);
int td_validate_parent(td_image_t *, td_image_t *);

void td_queue_write(td_image_t *, td_request_t);
void td_queue_read(td_image_t *, td_request_t);
void td_queue_block_status(td_image_t*, td_request_t*);
void td_forward_request(td_request_t);
void td_complete_request(td_request_t, int);

void td_debug(td_image_t *);

void td_queue_tiocb(td_driver_t *, struct tiocb *);
void td_prep_read(td_driver_t *, struct tiocb *, int, char *, size_t,
	long long, td_queue_callback_t, void *);
void td_prep_write(td_driver_t *, struct tiocb *, int, char *, size_t,
	long long, td_queue_callback_t, void *);
void td_panic(void) __noreturn;

#endif
