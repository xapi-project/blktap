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

#ifndef _TAPDISK_MESSAGE_H_
#define _TAPDISK_MESSAGE_H_

#include <inttypes.h>
#include <sys/types.h>

/*
 * TODO This is quite small since we don't allow path bigger than 256 chars. If
 * we ever increase this, make sure tapdisk_message_t structures are not
 * allocated on the stack.
 */
#define TAPDISK_MESSAGE_MAX_PATH_LENGTH  256
#define TAPDISK_MESSAGE_STRING_LENGTH    256

#define TAPDISK_MESSAGE_MAX_MINORS \
	((TAPDISK_MESSAGE_MAX_PATH_LENGTH / sizeof(int)) - 1)

#define TAPDISK_MESSAGE_FLAG_SHARED      0x001
#define TAPDISK_MESSAGE_FLAG_RDONLY      0x002
#define TAPDISK_MESSAGE_FLAG_ADD_CACHE   0x004
#define TAPDISK_MESSAGE_FLAG_VHD_INDEX   0x008
#define TAPDISK_MESSAGE_FLAG_LOG_DIRTY   0x010
#define TAPDISK_MESSAGE_FLAG_ADD_LCACHE  0x020
#define TAPDISK_MESSAGE_FLAG_REUSE_PRT   0x040
#define TAPDISK_MESSAGE_FLAG_SECONDARY   0x080
#define TAPDISK_MESSAGE_FLAG_STANDBY     0x100
#define TAPDISK_MESSAGE_FLAG_NO_O_DIRECT 0x200
#define TAPDISK_MESSAGE_FLAG_THIN        0x400

typedef struct tapdisk_message           tapdisk_message_t;
typedef uint32_t                         tapdisk_message_flag_t;
typedef struct tapdisk_message_image     tapdisk_message_image_t;
typedef struct tapdisk_message_params    tapdisk_message_params_t;
typedef struct tapdisk_message_string    tapdisk_message_string_t;
typedef struct tapdisk_message_response  tapdisk_message_response_t;
typedef struct tapdisk_message_minors    tapdisk_message_minors_t;
typedef struct tapdisk_message_list      tapdisk_message_list_t;
typedef struct tapdisk_message_stat      tapdisk_message_stat_t;

struct tapdisk_message_params {
	tapdisk_message_flag_t           flags;

	uint32_t                         devnum;
	uint32_t                         domid;
	char                             path[TAPDISK_MESSAGE_MAX_PATH_LENGTH];
	uint32_t                         prt_devnum;
	uint16_t                         req_timeout;
	char                             secondary[TAPDISK_MESSAGE_MAX_PATH_LENGTH];
};

struct tapdisk_message_image {
	uint64_t                         sectors;
	uint32_t                         sector_size;
	uint32_t                         info;
};

struct tapdisk_message_string {
	char                             text[TAPDISK_MESSAGE_STRING_LENGTH];
};

struct tapdisk_message_response {
	int                              error;
	char                             message[TAPDISK_MESSAGE_STRING_LENGTH];
};

struct tapdisk_message_minors {
	int                              count;
	int                              list[TAPDISK_MESSAGE_MAX_MINORS];
};

struct tapdisk_message_list {
	int                              count;
	int                              minor;
	int                              state;
	char                             path[TAPDISK_MESSAGE_MAX_PATH_LENGTH];
};

struct tapdisk_message_stat {
	uint16_t                         type;
	uint16_t                         cookie;
	size_t                           length;
};

/**
 * Tapdisk message containing all the necessary information required for the
 * tapdisk to connect to a guest's blkfront.
 */
typedef struct tapdisk_message_blkif {
	/**
	 * The domain ID of the guest to connect to.
	 */
	uint32_t domid;

	/**
	 * The device ID of the virtual block device.
	 */
	uint32_t devid;

	/**
	 * Grant references for the shared ring.
	 * TODO Why 8 specifically?
	 */
	uint32_t gref[8];

	/**
	 * Number of pages in the ring, expressed as a page order.
	 */
	uint32_t order;

	/**
	 * Protocol to use: native, 32 bit, or 64 bit. Used for supporting a
	 * 32-bit domU talking to a 64-bit dom0/domU and vice versa.
	 */
	uint32_t proto;

	/**
	 * TODO Page pool? Can be NULL.
	 */
	char pool[TAPDISK_MESSAGE_STRING_LENGTH];

	/**
	 * The event channel port.
	 */
	uint32_t port;
} tapdisk_message_blkif_t;

/**
 * Contains parameters for resuming a previously paused VBD.
 */
typedef struct tapdisk_message_resume {
    /**
     * TODO
     */
	tapdisk_message_flag_t flags;

    /**
     * A new VDI to use instead of the old one. Optional.
     */
    char new_params[TAPDISK_MESSAGE_MAX_PATH_LENGTH];

    /**
     * TODO
     */
    char secondary[TAPDISK_MESSAGE_MAX_PATH_LENGTH];
} tapdisk_message_resume_t;

struct tapdisk_message {
	uint16_t                       type;
	uint16_t                         cookie;

	union {
		pid_t                      tapdisk_pid;
		tapdisk_message_image_t    image;
		tapdisk_message_params_t   params;
		tapdisk_message_string_t   string;
		tapdisk_message_minors_t   minors;
		tapdisk_message_response_t response;
		tapdisk_message_list_t     list;
		tapdisk_message_stat_t     info;
		tapdisk_message_blkif_t    blkif;
        tapdisk_message_resume_t   resume;
	} u;
};

enum tapdisk_message_id {
	TAPDISK_MESSAGE_ERROR = 1,
	TAPDISK_MESSAGE_RUNTIME_ERROR,
	TAPDISK_MESSAGE_PID, /* 3 */
	TAPDISK_MESSAGE_PID_RSP,
	TAPDISK_MESSAGE_ATTACH,
	TAPDISK_MESSAGE_ATTACH_RSP,
	TAPDISK_MESSAGE_OPEN,
	TAPDISK_MESSAGE_OPEN_RSP,
	TAPDISK_MESSAGE_PAUSE, /* 9 */
	TAPDISK_MESSAGE_PAUSE_RSP,
	TAPDISK_MESSAGE_RESUME, /* 11 */
	TAPDISK_MESSAGE_RESUME_RSP,
	TAPDISK_MESSAGE_CLOSE,
	TAPDISK_MESSAGE_CLOSE_RSP,
	TAPDISK_MESSAGE_DETACH,
	TAPDISK_MESSAGE_DETACH_RSP,
	TAPDISK_MESSAGE_LIST_MINORS,
	TAPDISK_MESSAGE_LIST_MINORS_RSP,
	TAPDISK_MESSAGE_LIST, /* 19 */
	TAPDISK_MESSAGE_LIST_RSP,
	TAPDISK_MESSAGE_STATS,
	TAPDISK_MESSAGE_STATS_RSP,
	TAPDISK_MESSAGE_FORCE_SHUTDOWN,
	TAPDISK_MESSAGE_XENBLKIF_CONNECT,
	TAPDISK_MESSAGE_XENBLKIF_CONNECT_RSP,
	TAPDISK_MESSAGE_XENBLKIF_DISCONNECT, /* 26 */
	TAPDISK_MESSAGE_XENBLKIF_DISCONNECT_RSP,
	TAPDISK_MESSAGE_DISK_INFO,
	TAPDISK_MESSAGE_DISK_INFO_RSP,
	TAPDISK_MESSAGE_EXIT,
};

#define TAPDISK_MESSAGE_MAX TAPDISK_MESSAGE_EXIT

static inline char *
tapdisk_message_name(enum tapdisk_message_id id)
{
	switch (id) {
	case TAPDISK_MESSAGE_ERROR:
		return "error";

	case TAPDISK_MESSAGE_PID:
		return "pid";

	case TAPDISK_MESSAGE_PID_RSP:
		return "pid response";

	case TAPDISK_MESSAGE_OPEN:
		return "open";

	case TAPDISK_MESSAGE_OPEN_RSP:
		return "open response";

	case TAPDISK_MESSAGE_PAUSE:
		return "pause";

	case TAPDISK_MESSAGE_PAUSE_RSP:
		return "pause response";

	case TAPDISK_MESSAGE_RESUME:
		return "resume";

	case TAPDISK_MESSAGE_RESUME_RSP:
		return "resume response";

	case TAPDISK_MESSAGE_CLOSE:
		return "close";

	case TAPDISK_MESSAGE_FORCE_SHUTDOWN:
		return "force shutdown";

	case TAPDISK_MESSAGE_CLOSE_RSP:
		return "close response";

	case TAPDISK_MESSAGE_ATTACH:
		return "attach";

	case TAPDISK_MESSAGE_ATTACH_RSP:
		return "attach response";

	case TAPDISK_MESSAGE_DETACH:
		return "detach";

	case TAPDISK_MESSAGE_DETACH_RSP:
		return "detach response";

	case TAPDISK_MESSAGE_LIST_MINORS:
		return "list minors";

	case TAPDISK_MESSAGE_LIST_MINORS_RSP:
		return "list minors response";

	case TAPDISK_MESSAGE_LIST:
		return "list";

	case TAPDISK_MESSAGE_LIST_RSP:
		return "list response";

	case TAPDISK_MESSAGE_STATS:
		return "stats";

	case TAPDISK_MESSAGE_STATS_RSP:
		return "stats response";

	case TAPDISK_MESSAGE_XENBLKIF_CONNECT:
		return "sring connect";

	case TAPDISK_MESSAGE_XENBLKIF_CONNECT_RSP:
		return "sring connect rsp";

	case TAPDISK_MESSAGE_XENBLKIF_DISCONNECT:
		return "sring disconnect";

	case TAPDISK_MESSAGE_XENBLKIF_DISCONNECT_RSP:
		return "sring disconnect rsp";

	case TAPDISK_MESSAGE_DISK_INFO:
		return "disk info";

	case TAPDISK_MESSAGE_DISK_INFO_RSP:
		return "disk info rsp";

	case TAPDISK_MESSAGE_EXIT:
		return "exit";

	default:
		return "unknown";
	}
}

#endif
