/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _TAPDISK_SERVER_H_
#define _TAPDISK_SERVER_H_

#include "list.h"
#include "tapdisk-vbd.h"
#include "tapdisk-queue.h"

enum memory_mode_t {NORMAL_MEMORY_MODE, LOW_MEMORY_MODE};

enum memory_mode_t tapdisk_server_mem_mode(void);

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
void tapdisk_server_start_polling(void);
void tapdisk_server_stop_polling(void);

#endif
