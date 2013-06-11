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

#ifndef _TAPDISK_VBD_H_
#define _TAPDISK_VBD_H_

#include <sys/time.h>

#include "tapdisk.h"
#include "scheduler.h"
#include "tapdisk-image.h"
#include "tapdisk-blktap.h"

#define TD_VBD_REQUEST_TIMEOUT      120
#define TD_VBD_MAX_RETRIES          100
#define TD_VBD_RETRY_INTERVAL       1

#define TD_VBD_DEAD                 0x0001
#define TD_VBD_CLOSED               0x0002
#define TD_VBD_QUIESCE_REQUESTED    0x0004
#define TD_VBD_QUIESCED             0x0008
#define TD_VBD_PAUSE_REQUESTED      0x0010
#define TD_VBD_PAUSED               0x0020
#define TD_VBD_SHUTDOWN_REQUESTED   0x0040
#define TD_VBD_LOCKING              0x0080
#define TD_VBD_LOG_DROPPED          0x0100

#define TD_VBD_SECONDARY_DISABLED   0 
#define TD_VBD_SECONDARY_MIRROR     1
#define TD_VBD_SECONDARY_STANDBY    2

struct td_nbdserver;

struct td_vbd_handle {
	char                       *name;

	td_blktap_t                *tap;

	td_uuid_t                   uuid;

	td_flag_t                   flags;
	td_flag_t                   state;

	struct list_head            images;

	int                         parent_devnum;
	char                       *secondary_name;
	td_image_t                 *secondary;
	uint8_t                     secondary_mode;

	int                         FIXME_enospc_redirect_count_enabled;
	uint64_t                    FIXME_enospc_redirect_count;

	/*
	 * when we encounter ENOSPC on the primary leaf image in mirror mode, 
	 * we need to remove it from the VBD chain so that writes start going 
	 * on the secondary leaf. However, we cannot free the image at that 
	 * time since it might still have in-flight treqs referencing it.  
	 * Therefore, we move it into 'retired' until shutdown.
	 */
	td_image_t                 *retired;

	int                         nbd_mirror_failed;

	struct list_head            new_requests;
	struct list_head            pending_requests;
	struct list_head            failed_requests;
	struct list_head            completed_requests;

	td_vbd_request_t            request_list[MAX_REQUESTS]; /* XXX */

	struct list_head            next;

	uint16_t                    req_timeout; /* in seconds */
	struct timeval              ts;

	uint64_t                    received;
	uint64_t                    returned;
	uint64_t                    kicked;
	uint64_t                    secs_pending;
	uint64_t                    retries;
	uint64_t                    errors;
	td_sector_count_t           secs;

	struct td_nbdserver        *nbdserver;
};

#define tapdisk_vbd_for_each_request(vreq, tmp, list)	                \
	list_for_each_entry_safe((vreq), (tmp), (list), next)

#define tapdisk_vbd_for_each_image(vbd, image, tmp)	\
	tapdisk_for_each_image_safe(image, tmp, &vbd->images)

static inline void
tapdisk_vbd_move_request(td_vbd_request_t *vreq, struct list_head *dest)
{
	list_del(&vreq->next);
	INIT_LIST_HEAD(&vreq->next);
	list_add_tail(&vreq->next, dest);
	vreq->list_head = dest;
}

static inline void
tapdisk_vbd_add_image(td_vbd_t *vbd, td_image_t *image)
{
	list_add_tail(&image->next, &vbd->images);
}

static inline int
tapdisk_vbd_is_last_image(td_vbd_t *vbd, td_image_t *image)
{
	return list_is_last(&image->next, &vbd->images);
}

static inline td_image_t *
tapdisk_vbd_first_image(td_vbd_t *vbd)
{
	td_image_t *image = NULL;
	if (!list_empty(&vbd->images))
		image = list_entry(vbd->images.next, td_image_t, next);
	return image;
}

static inline td_image_t *
tapdisk_vbd_last_image(td_vbd_t *vbd)
{
	td_image_t *image = NULL;
	if (!list_empty(&vbd->images))
		image = list_entry(vbd->images.prev, td_image_t, next);
	return image;
}

static inline td_image_t *
tapdisk_vbd_next_image(td_image_t *image)
{
	return list_entry(image->next.next, td_image_t, next);
}

td_vbd_t *tapdisk_vbd_create(td_uuid_t);
int tapdisk_vbd_initialize(int, int, td_uuid_t);
int tapdisk_vbd_open(td_vbd_t *, const char *, int, const char *, td_flag_t);
int tapdisk_vbd_close(td_vbd_t *);

int tapdisk_vbd_open_vdi(td_vbd_t *, const char *, td_flag_t, int);
void tapdisk_vbd_close_vdi(td_vbd_t *);

int tapdisk_vbd_attach(td_vbd_t *, const char *, int);
void tapdisk_vbd_detach(td_vbd_t *);

int tapdisk_vbd_queue_request(td_vbd_t *, td_vbd_request_t *);
void tapdisk_vbd_forward_request(td_request_t);

int tapdisk_vbd_get_disk_info(td_vbd_t *, td_disk_info_t *);
int tapdisk_vbd_retry_needed(td_vbd_t *);
int tapdisk_vbd_quiesce_queue(td_vbd_t *);
int tapdisk_vbd_start_queue(td_vbd_t *);
int tapdisk_vbd_issue_requests(td_vbd_t *);
int tapdisk_vbd_kill_queue(td_vbd_t *);
int tapdisk_vbd_pause(td_vbd_t *);
int tapdisk_vbd_resume(td_vbd_t *, const char *);
void tapdisk_vbd_kick(td_vbd_t *);
void tapdisk_vbd_check_state(td_vbd_t *);
int tapdisk_vbd_recheck_state(td_vbd_t *);
void tapdisk_vbd_check_progress(td_vbd_t *);
void tapdisk_vbd_debug(td_vbd_t *);
int tapdisk_vbd_start_nbdserver(td_vbd_t *);
void tapdisk_vbd_stats(td_vbd_t *, td_stats_t *);

#endif
