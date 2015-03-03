/* 
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef _TAPDISK_INTERFACE_H_
#define _TAPDISK_INTERFACE_H_

#include "tapdisk.h"
#include "tapdisk-queue.h"
#include "tapdisk-image.h"
#include "tapdisk-driver.h"

int td_open(td_image_t *);
int __td_open(td_image_t *, td_disk_info_t *);
int td_load(td_image_t *);
int td_close(td_image_t *);
int td_get_oflags(td_image_t *, int *);
int td_get_parent_id(td_image_t *, td_disk_id_t *);
int td_validate_parent(td_image_t *, td_image_t *);

void td_queue_write(td_image_t *, td_request_t);
void td_queue_read(td_image_t *, td_request_t);
void td_forward_request(td_request_t);
void td_complete_request(td_request_t, int);

void td_debug(td_image_t *);

void td_queue_tiocb(td_driver_t *, struct tiocb *);
void td_prep_read(struct tiocb *, int, char *, size_t,
		  long long, td_queue_callback_t, void *);
void td_prep_write(struct tiocb *, int, char *, size_t,
		   long long, td_queue_callback_t, void *);
void td_panic(void) __noreturn;

#endif
