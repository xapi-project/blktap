/*
 * Copyright (c) 2026, Cloud Software Group, Inc.
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

/*
 * Unit tests for the guest-controlled blkif request validation in td-req.c
 * (CA-429650). The functions exercised (tapdisk_xenblkif_make_vbd_request /
 * tapdisk_xenblkif_parse_request) are static inline, so the implementation is
 * included directly and the handful of externs it references are stubbed - none
 * of them are reached on the validation paths under test.
 */

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <unistd.h>

/* Pull in the code under test, including its static functions. */
#include "td-req.c"

/* --- stubs for the externs td-req.c links against (never called here) --- */

unsigned int PAGE_SIZE;
unsigned int PAGE_SHIFT;

event_id_t
tapdisk_server_register_event(char m, int fd, struct timeval t,
		event_cb_t cb, void *p)
{
	return 0;
}

void
tapdisk_server_unregister_event(event_id_t id)
{
}

int
tapdisk_vbd_queue_request(td_vbd_t *vbd, td_vbd_request_t *vreq)
{
	return 0;
}

int
tapdisk_xenblkif_destroy(struct td_xenblkif *blkif)
{
	return 0;
}

int
tapdisk_xenblkif_reqs_pending(const struct td_xenblkif * const blkif)
{
	return 0;
}

void
tapdisk_xenblkif_sched_chkrng(const struct td_xenblkif *blkif)
{
}

bool
tapdisk_xenblkif_barrier_should_complete(const struct td_xenblkif * const blkif)
{
	return false;
}

int
xenevtchn_notify(xenevtchn_handle *xce, evtchn_port_t port)
{
	return 0;
}

/* --- helpers --- */

static void
init_read_request(struct td_xenblkif_req *req, uint8_t nr_segments)
{
	int i;

	memset(req, 0, sizeof(*req));
	req->msg.operation = BLKIF_OP_READ;
	req->msg.nr_segments = nr_segments;

	/* One valid, in-page sector per segment. */
	for (i = 0; i < nr_segments; i++) {
		req->msg.seg[i].first_sect = 0;
		req->msg.seg[i].last_sect = 0;
	}
}

/* --- tests --- */

/*
 * A request whose segment count exceeds the ring descriptor's seg[] capacity
 * must be rejected before any segment is dereferenced.
 */
static void
test_reject_too_many_segments(void **state)
{
	struct td_xenblkif blkif;
	struct td_xenblkif_req req;

	memset(&blkif, 0, sizeof(blkif));
	init_read_request(&req, BLKIF_MAX_SEGMENTS_PER_REQUEST + 1);

	assert_int_equal(tapdisk_xenblkif_make_vbd_request(&blkif, &req), EINVAL);
}

/* A read request with no segments is not a valid transfer and is rejected. */
static void
test_reject_zero_segments(void **state)
{
	struct td_xenblkif blkif;
	struct td_xenblkif_req req;

	memset(&blkif, 0, sizeof(blkif));
	init_read_request(&req, 0);

	assert_int_equal(tapdisk_xenblkif_make_vbd_request(&blkif, &req), EINVAL);
}

/* The maximum legal segment count is accepted and parsed. */
static void
test_accept_max_segments(void **state)
{
	struct td_xenblkif blkif;
	struct td_xenblkif_req req;

	memset(&blkif, 0, sizeof(blkif));
	init_read_request(&req, BLKIF_MAX_SEGMENTS_PER_REQUEST);

	assert_int_equal(tapdisk_xenblkif_make_vbd_request(&blkif, &req), 0);

	if (req.vma)
		munmap(req.vma, (size_t)TD_REQ_BUFFER_SIZE);
}

/* last_sect beyond the end of the page is rejected (would over-read/over-copy). */
static void
test_reject_last_sect_out_of_page(void **state)
{
	struct td_xenblkif blkif;
	struct td_xenblkif_req req;

	memset(&blkif, 0, sizeof(blkif));
	init_read_request(&req, 1);
	req.msg.seg[0].first_sect = 0;
	req.msg.seg[0].last_sect = 1U << (PAGE_SHIFT - SECTOR_SHIFT); /* == 8 */

	assert_int_equal(tapdisk_xenblkif_make_vbd_request(&blkif, &req), EINVAL);

	if (req.vma)
		munmap(req.vma, (size_t)TD_REQ_BUFFER_SIZE);
}

/* last_sect < first_sect (an inverted range) is rejected. */
static void
test_reject_inverted_sectors(void **state)
{
	struct td_xenblkif blkif;
	struct td_xenblkif_req req;

	memset(&blkif, 0, sizeof(blkif));
	init_read_request(&req, 1);
	req.msg.seg[0].first_sect = 4;
	req.msg.seg[0].last_sect = 2;

	assert_int_equal(tapdisk_xenblkif_make_vbd_request(&blkif, &req), EINVAL);

	if (req.vma)
		munmap(req.vma, (size_t)TD_REQ_BUFFER_SIZE);
}

/*
 * Regression guard for the sizing that made the above bounds meaningful: the
 * validation bound, the per-request buffer and the vectorised arrays must all
 * be able to hold exactly the ring descriptor's seg[] capacity. Historically
 * they diverged (arrays sized 32 while seg[] holds 11), which is what allowed
 * the out-of-bounds accesses.
 */
static void
test_segment_arrays_match_ring_capacity(void **state)
{
	struct td_xenblkif_req req;

	assert_int_equal(ARRAY_SIZE(req.msg.seg), BLKIF_MAX_SEGMENTS_PER_REQUEST);
	assert_int_equal(ARRAY_SIZE(req.gcopy_segs), ARRAY_SIZE(req.msg.seg));
	assert_int_equal(ARRAY_SIZE(req.iov), ARRAY_SIZE(req.msg.seg));
	assert_int_equal(ARRAY_SIZE(req.gref), ARRAY_SIZE(req.msg.seg));
	assert_int_equal((size_t)TD_REQ_BUFFER_SIZE,
			(size_t)ARRAY_SIZE(req.msg.seg) << PAGE_SHIFT);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_reject_too_many_segments),
		cmocka_unit_test(test_reject_zero_segments),
		cmocka_unit_test(test_accept_max_segments),
		cmocka_unit_test(test_reject_last_sect_out_of_page),
		cmocka_unit_test(test_reject_inverted_sectors),
		cmocka_unit_test(test_segment_arrays_match_ring_capacity),
	};

	/* td-req.c reads PAGE_SIZE/PAGE_SHIFT, normally set by the server. */
	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	for (PAGE_SHIFT = 0; (1U << PAGE_SHIFT) < PAGE_SIZE; PAGE_SHIFT++)
		;

	return cmocka_run_group_tests_name("td-req request validation",
			tests, NULL, NULL);
}
