/*
 * Copyright (c) 2011, XenSource Inc.
 * All rights reserved.
 */

#ifndef _LINUX_BLKTAP_H
#define _LINUX_BLKTAP_H

/*
 * Control
 */

#define BLKTAP_IOCTL_RESPOND        1
#define BLKTAP_IOCTL_ALLOC_TAP      200
#define BLKTAP_IOCTL_FREE_TAP       201
#define BLKTAP_IOCTL_CREATE_DEVICE  208
#define BLKTAP_IOCTL_REMOVE_DEVICE  207

struct blktap_info {
	unsigned int            ring_major;
	unsigned int            bdev_major;
	unsigned int            ring_minor;
};

struct blktap_device_info {
	unsigned long long      capacity;
	unsigned int            sector_size;
	unsigned int            physical_sector_size;
	unsigned long           flags;
	unsigned long           __rsvd[4];
};

#define BLKTAP_DEVICE_RO        0x00000001UL

/*
 * I/O ring
 */

#ifdef __KERNEL__
#include <asm/page.h>
#define BLKTAP_PAGE_SIZE PAGE_SIZE

#include <linux/log2.h>
#define BLKTAP_RD32(_n) rounddown_pow_of_two(_n)
#endif

#define __BLKTAP_RING_SIZE(_sz)					\
	((unsigned int)						\
	 BLKTAP_RD32(((_sz) - offsetof(struct blktap_sring, entry)) /	\
		     sizeof(union blktap_ring_entry)))

typedef struct blktap_ring_request  blktap_ring_req_t;
typedef struct blktap_ring_response blktap_ring_rsp_t;

struct blktap_segment {
	uint32_t                __pad;
	uint8_t                 first_sect;
	uint8_t                 last_sect;
};

#define BLKTAP_OP_READ          0
#define BLKTAP_OP_WRITE         1

#define BLKTAP_SEGMENT_MAX      11

struct blktap_ring_request {
	uint8_t                 operation;
	uint8_t                 nr_segments;
	uint16_t                __pad;
	uint64_t                id;
	uint64_t                sector_number;
	struct blktap_segment   seg[BLKTAP_SEGMENT_MAX];
};

#define BLKTAP_RSP_EOPNOTSUPP  -2
#define BLKTAP_RSP_ERROR       -1
#define BLKTAP_RSP_OKAY         0

struct blktap_ring_response {
	uint64_t                id;
	uint8_t                 operation;
	int16_t                 status;
};

union blktap_ring_entry {
	struct blktap_ring_request  req;
	struct blktap_ring_response rsp;
};

struct blktap_sring {
	uint32_t req_prod;
	uint32_t __req_event;

	uint32_t rsp_prod;
	uint32_t __rsp_event;

	uint8_t  msg;
	uint8_t  __rsvd[47];

	union blktap_ring_entry entry[0];
};

/*
 * Ring messages + old ioctls (DEPRECATED)
 */

#define BLKTAP_RING_MESSAGE_CLOSE   3
#define BLKTAP_IOCTL_CREATE_DEVICE_COMPAT 202
#define BLKTAP_NAME_MAX 256

struct blktap2_params {
	char               name[BLKTAP_NAME_MAX];
	unsigned long long capacity;
	unsigned long      sector_size;
};

#endif /* _LINUX_BLKTAP_H */
