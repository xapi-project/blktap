/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "aio_getevents.h"
#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/likely.h"
#include "spdk/util.h"

/* For user space reaping of completions */
struct spdk_aio_ring {
	uint32_t id;
	uint32_t size;
	uint32_t head;
	uint32_t tail;

	uint32_t version;
	uint32_t compat_features;
	uint32_t incompat_features;
	uint32_t header_length;
};

#define SPDK_AIO_RING_VERSION	0xa10a10a1

int user_io_getevents(io_context_t io_ctx, unsigned int max, struct io_event *uevents)
{
	uint32_t head, tail, count;
	struct spdk_aio_ring *ring;
	struct timespec timeout;
	struct io_event *kevents;

	ring = (struct spdk_aio_ring *)io_ctx;

	if (spdk_unlikely(ring->version != SPDK_AIO_RING_VERSION || ring->incompat_features != 0)) {
		timeout.tv_sec = 0;
		timeout.tv_nsec = 0;

		return io_getevents(io_ctx, 0, max, uevents, &timeout);
	}

	/* Read the current state out of the ring */
	head = ring->head;
	tail = ring->tail;

	/* This memory barrier is required to prevent the loads above
	 * from being re-ordered with stores to the events array
	 * potentially occurring on other threads. */
	spdk_smp_rmb();

	/* Calculate how many items are in the circular ring */
	count = tail - head;
	if (tail < head) {
		count += ring->size;
	}

	/* Reduce the count to the limit provided by the user */
	count = spdk_min(max, count);

	/* Grab the memory location of the event array */
	kevents = (struct io_event *)((uintptr_t)ring + ring->header_length);

	/* Copy the events out of the ring. */
	if ((head + count) <= ring->size) {
		/* Only one copy is required */
		memcpy(uevents, &kevents[head], count * sizeof(struct io_event));
	} else {
		uint32_t first_part = ring->size - head;
		/* Two copies are required */
		memcpy(uevents, &kevents[head], first_part * sizeof(struct io_event));
		memcpy(&uevents[first_part], &kevents[0], (count - first_part) * sizeof(struct io_event));
	}

	/* Update the head pointer. On x86, stores will not be reordered with older loads,
	 * so the copies out of the event array will always be complete prior to this
	 * update becoming visible. On other architectures this is not guaranteed, so
	 * add a barrier. */
#if defined(__i386__) || defined(__x86_64__)
	spdk_compiler_barrier();
#else
	spdk_smp_mb();
#endif
	ring->head = (head + count) % ring->size;

	return count;
}
