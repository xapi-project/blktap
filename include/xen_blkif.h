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

#ifndef __XEN_BLKIF_H__
#define __XEN_BLKIF_H__

#include <xen/io/ring.h>
#include <xen/io/blkif.h>
#include <xen/io/protocols.h>

/* Not a real protocol.  Used to generate ring structs which contain
 * the elements common to all protocols only.  This way we get a
 * compiler-checkable way to use common struct elements, so we can
 * avoid using switch(protocol) in a number of places.  */
struct blkif_common_request {
	char dummy;
};
struct blkif_common_response {
	char dummy;
};

typedef union blkif_request_common blkif_request_common_t;

/* i386 protocol version */
#pragma pack(push, 4)
struct blkif_x86_32_request {
	uint8_t        operation;    /* BLKIF_OP_???                         */
	uint8_t        nr_segments;  /* number of segments                   */
	blkif_vdev_t   handle;       /* only for read/write requests         */
	uint64_t       id;           /* private guest value, echoed in resp  */
	blkif_sector_t sector_number;/* start sector idx on disk (r/w only)  */
	struct blkif_request_segment seg[BLKIF_MAX_SEGMENTS_PER_REQUEST];
};
struct blkif_x86_32_request_discard {
	uint8_t        operation;    /* BLKIF_OP_???                         */
	uint8_t        flags;        /* BLKIF_DISCARD_SECURE or zero         */
	blkif_vdev_t   handle;       /* only for read/write requests         */
	uint64_t       id;           /* private guest value, echoed in resp  */
	blkif_sector_t sector_number;/* start sector idx on disk (r/w only)  */
	uint64_t       nr_sectors;   /* number of contiguous sectors to discard */
};
struct blkif_x86_32_response {
	uint64_t        id;              /* copied from request */
	uint8_t         operation;       /* copied from request */
	int16_t         status;          /* BLKIF_RSP_???       */
};
typedef struct blkif_x86_32_request blkif_x86_32_request_t;
typedef struct blkif_x86_32_request_discard blkif_x86_32_request_discard_t;
typedef struct blkif_x86_32_response blkif_x86_32_response_t;
#pragma pack(pop)

/* x86_64 protocol version */
struct blkif_x86_64_request {
	uint8_t        operation;    /* BLKIF_OP_???                         */
	uint8_t        nr_segments;  /* number of segments                   */
	blkif_vdev_t   handle;       /* only for read/write requests         */
	uint64_t       __attribute__((__aligned__(8))) id;
	blkif_sector_t sector_number;/* start sector idx on disk (r/w only)  */
	struct blkif_request_segment seg[BLKIF_MAX_SEGMENTS_PER_REQUEST];
};
struct blkif_x86_64_request_discard {
	uint8_t        operation;    /* BLKIF_OP_???                         */
	uint8_t        flags;        /* BLKIF_DISCARD_SECURE or zero         */
	blkif_vdev_t   handle;       /* only for read/write requests         */
	uint32_t       pad;
	uint64_t       id;           /* private guest value, echoed in resp  */
	blkif_sector_t sector_number;/* start sector idx on disk (r/w only)  */
	uint64_t       nr_sectors;   /* number of contiguous sectors to discard */
};
struct blkif_x86_64_response {
	uint64_t       __attribute__((__aligned__(8))) id;
	uint8_t         operation;       /* copied from request */
	int16_t         status;          /* BLKIF_RSP_???       */
};
typedef struct blkif_x86_64_request blkif_x86_64_request_t;
typedef struct blkif_x86_64_request_discard blkif_x86_64_request_discard_t;
typedef struct blkif_x86_64_response blkif_x86_64_response_t;

DEFINE_RING_TYPES(blkif_common, struct blkif_common_request, struct blkif_common_response);
DEFINE_RING_TYPES(blkif_x86_32, struct blkif_x86_32_request, struct blkif_x86_32_response);
DEFINE_RING_TYPES(blkif_x86_64, struct blkif_x86_64_request, struct blkif_x86_64_response);

union blkif_back_rings {
	blkif_back_ring_t        native;
	blkif_common_back_ring_t common;
        blkif_x86_32_back_ring_t x86_32;
        blkif_x86_64_back_ring_t x86_64;
};
typedef union blkif_back_rings blkif_back_rings_t;

enum blkif_protocol {
	BLKIF_PROTOCOL_NATIVE = 1,
	BLKIF_PROTOCOL_X86_32 = 2,
	BLKIF_PROTOCOL_X86_64 = 3,
};

static inline void blkif_get_x86_32_req(blkif_request_t *dst, blkif_x86_32_request_t *src)
{
	int i, n = BLKIF_MAX_SEGMENTS_PER_REQUEST;

	dst->operation = src->operation;
	dst->nr_segments = src->nr_segments;
	dst->handle = src->handle;
	dst->id = src->id;
	dst->sector_number = src->sector_number;
	if (n > src->nr_segments)
		n = src->nr_segments;
	for (i = 0; i < n; i++)
		dst->seg[i] = src->seg[i];
}

static inline void blkif_get_x86_64_req(blkif_request_t *dst, blkif_x86_64_request_t *src)
{
	int i, n = BLKIF_MAX_SEGMENTS_PER_REQUEST;

	dst->operation = src->operation;
	dst->nr_segments = src->nr_segments;
	dst->handle = src->handle;
	dst->id = src->id;
	dst->sector_number = src->sector_number;
	if (n > src->nr_segments)
		n = src->nr_segments;
	for (i = 0; i < n; i++)
		dst->seg[i] = src->seg[i];
}

static inline void
blkif_get_x86_32_req_discard(blkif_request_discard_t *dst,
							 blkif_x86_32_request_discard_t *src)
{
	dst->operation = src->operation;
	dst->nr_sectors = src->nr_sectors;
	dst->handle = src->handle;
	dst->id = src->id;
	dst->sector_number = src->sector_number;
}

static inline void blkif_get_x86_64_req_discard(blkif_request_discard_t *dst,
												blkif_x86_64_request_discard_t *src)
{
	dst->operation = src->operation;
	dst->nr_sectors = src->nr_sectors;
	dst->handle = src->handle;
	dst->id = src->id;
	dst->sector_number = src->sector_number;
}
#endif /* __XEN_BLKIF_H__ */
