/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TAPDISK_SERVER_H_
#define _TAPDISK_SERVER_H_

#include "list.h"
#include "tapdisk-vbd.h"
#include "io-backend.h"

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

typedef void (*q_tiocb)(struct tiocb *);
typedef void (*p_tiocb)(struct tiocb *, int, int, char *, size_t,
	long long, td_queue_callback_t, void *);

void tapdisk_server_queue_tiocb_ro(struct tiocb *);
void tapdisk_server_prep_tiocb_ro(struct tiocb *, int, int, char *, size_t,
	long long, td_queue_callback_t, void *);
void tapdisk_server_queue_tiocb(struct tiocb *);
void tapdisk_server_prep_tiocb(struct tiocb *, int, int, char *, size_t,
	long long, td_queue_callback_t, void *);

void tapdisk_server_check_state(void);

event_id_t tapdisk_server_register_event(char, int, struct timeval, event_cb_t, void *);
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

int tapdisk_server_event_set_timeout(event_id_t, struct timeval timeo);

float tapdisk_server_system_idle_cpu(void);

#endif
