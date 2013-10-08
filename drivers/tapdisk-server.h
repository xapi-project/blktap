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

#ifndef _TAPDISK_SERVER_H_
#define _TAPDISK_SERVER_H_

#include "list.h"
#include "tapdisk-vbd.h"
#include "tapdisk-queue.h"

struct tap_disk *tapdisk_server_find_driver_interface(int);

td_image_t *tapdisk_server_get_shared_image(td_image_t *);

struct list_head *tapdisk_server_get_all_vbds(void);

/**
 * Returns the VBD that corresponds to the specified minor.
 * Returns NULL if such a VBD does not exist.
 */
td_vbd_t *tapdisk_server_get_vbd(td_uuid_t);

/**
 * Adds the VBD to end of the list of VBDs.
 */
void tapdisk_server_add_vbd(td_vbd_t *);

/**
 * Removes the VBDs from the list of VBDs.
 */
void tapdisk_server_remove_vbd(td_vbd_t *);

void tapdisk_server_queue_tiocb(struct tiocb *);

void tapdisk_server_check_state(void);

event_id_t tapdisk_server_register_event(char, int, int, event_cb_t, void *);
void tapdisk_server_unregister_event(event_id_t);
void tapdisk_server_mask_event(event_id_t, int);
void tapdisk_server_set_max_timeout(int);

int tapdisk_server_init(void);
int tapdisk_server_initialize(const char *, const char *);
int tapdisk_server_complete(void);
int tapdisk_server_run(void);
void tapdisk_server_iterate(void);

int tapdisk_server_openlog(const char *, int, int);
void tapdisk_server_closelog(void);
void tapdisk_start_logging(const char *, const char *);
void tapdisk_stop_logging(void);

#endif
