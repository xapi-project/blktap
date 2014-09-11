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
#include "td-blkif.h"

#define TD_VBD_REQUEST_TIMEOUT      120
#define TD_VBD_MAX_RETRIES          100
#define TD_VBD_RETRY_INTERVAL       1

/*
 * VBD states
 */
#define TD_VBD_DEAD                 0x0001
#define TD_VBD_CLOSED               0x0002
#define TD_VBD_QUIESCE_REQUESTED    0x0004
#define TD_VBD_QUIESCED             0x0008
#define TD_VBD_PAUSE_REQUESTED      0x0010
#define TD_VBD_PAUSED               0x0020
#define TD_VBD_SHUTDOWN_REQUESTED   0x0040
#define TD_VBD_LOCKING              0x0080
#define TD_VBD_LOG_DROPPED          0x0100
#define TD_VBD_RESUME_FAILED        0x0200

#define TD_VBD_SECONDARY_DISABLED   0
#define TD_VBD_SECONDARY_MIRROR     1
#define TD_VBD_SECONDARY_STANDBY    2

struct td_nbdserver;

struct td_vbd_rrd {

    struct shm shm;

    /*
     * Previous value of td_vbd_handle.errors. We maintain this in order to
     * tell whether we need to update the RRD.
     */
    uint64_t last_errors;

	time_t last;
};

struct td_vbd_handle {
    /**
     * type:/path/to/file
     */
	char                       *name;

	td_blktap_t                *tap;

	td_uuid_t                   uuid;

    /**
     * shared rings
     */
    struct list_head           rings;

    /**
     * List of rings that contain pending requests but a disconnection was
     * issued. We need to maintain these rings until all their pending requests
     * complete. When the last request completes, the ring is destroyed and
     * removed from this list.
     */
    struct list_head            dead_rings;

	td_flag_t                   flags;

	/**
	 * VBD state (TD_VBD_XXX, excluding SECONDARY and request-related)
	 */
	td_flag_t                   state;

	/**
	 * List of images: the leaf is at the head, the tree root is at the tail.
	 */
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

	/**
	 * We keep a copy of the disk info because we might receive a disk info
	 * request while we're in the paused state.
	 */
	td_disk_info_t              disk_info;

    struct td_vbd_rrd           rrd;
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

td_vbd_t *tapdisk_vbd_create(td_uuid_t);
int tapdisk_vbd_initialize(int, int, td_uuid_t);
int tapdisk_vbd_open(td_vbd_t *, const char *, int, const char *, td_flag_t);
int tapdisk_vbd_close(td_vbd_t *);

/**
 * Opens a VDI.
 *
 * @params vbd output parameter that receives a handle to the opened VDI
 * @param params type:/path/to/file
 * @params flags TD_OPEN_* TODO which TD_OPEN_* flags are honored? How does
 * each flag affect the behavior of this functions? Move TD_OPEN_* flag
 * definitions close to this function (check if they're used only by this
 * function)?
 * @param prt_devnum parent minor (optional)
 * @returns 0 on success
 */
int tapdisk_vbd_open_vdi(td_vbd_t * vbd, const char *params, td_flag_t flags,
        int prt_devnum);
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

/**
 * Checks whether there are new requests and if so it submits them, prodived
 * that the queue has not been quiesced.
 *
 * Returns 1 if new requests have been issued, otherwise it returns 0.
 */
int tapdisk_vbd_recheck_state(td_vbd_t *);

void tapdisk_vbd_check_progress(td_vbd_t *);
void tapdisk_vbd_debug(td_vbd_t *);
int tapdisk_vbd_start_nbdserver(td_vbd_t *);
void tapdisk_vbd_stats(td_vbd_t *, td_stats_t *);

/**
 * Tells whether the VBD contains at least one dead ring.
 */
bool inline tapdisk_vbd_contains_dead_rings(td_vbd_t * vbd);
#endif
