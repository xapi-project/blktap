/*
 * Copyright (c) 2020, Citrix Systems, Inc.
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

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>

#include "test-suites.h"
#include "tapdisk.h"
#include "tapdisk-vbd.h"
#include "tapdisk-disktype.h"
#include "tapdisk-image.h"
#include "tapdisk-interface.h"

void
test_vbd_linked_list(void **state)
{
	tapdisk_extents_t extents;
	bzero(&extents, sizeof(tapdisk_extents_t));
	td_request_t vreq;
	bzero(&vreq, sizeof(td_request_t));

	vreq.sec = 0;
	vreq.secs = 2;
	vreq.status = 1;
	
	add_extent(&extents, &vreq);
	assert_ptr_equal(extents.tail, extents.head);
	assert_int_equal(extents.tail->start, 0);
	assert_int_equal(extents.tail->length, 2);
	assert_int_equal(extents.tail->flag, 1);
	assert_null(extents.tail->next);
	assert_int_equal(extents.count, 1);
}

void
test_vbd_issue_request(void **stat)
{

	td_vbd_t vbd;
	bzero(&vbd, sizeof(td_vbd_t));
	INIT_LIST_HEAD(&vbd.images);
	INIT_LIST_HEAD(&vbd.pending_requests);
	td_image_t *image = tapdisk_image_allocate("blah", DISK_TYPE_VHD, TD_OPEN_RDONLY | TD_OPEN_SHAREABLE);
	list_add_tail(&image->next, &vbd.images);

	td_vbd_request_t vreq;
	bzero(&vreq, sizeof(td_vbd_request_t));
	INIT_LIST_HEAD(&vreq.next);
	vreq.iovcnt = 1;
	struct td_iovec iov;
	iov.base = 0;
	iov.secs = 2;
	vreq.iov = &iov;
	vreq.op = TD_OP_BLOCK_STATUS;

	td_request_t my_treq;
	bzero(&my_treq, sizeof(my_treq));
	my_treq.sidx           = 0;
	my_treq.buf            = iov.base;
	my_treq.sec            = vreq.sec;
	my_treq.secs           = iov.secs;
	my_treq.image          = image;
	my_treq.cb_data        = NULL;
	my_treq.vreq           = &vreq;
	my_treq.op = TD_OP_BLOCK_STATUS;
	my_treq.cb = tapdisk_vbd_complete_block_status_request;
	
	expect_memory(__wrap_td_queue_block_status, treq, &my_treq, sizeof(my_treq));
	
	will_return(__wrap_tapdisk_image_check_request, 0);
	int err = tapdisk_vbd_issue_request(&vbd, &vreq);
	assert_int_equal(err, 0);
	tapdisk_image_free(image);
}

void
test_vbd_complete_block_status_request(void **stat)
{

	td_vbd_t vbd;
	bzero(&vbd, sizeof(td_vbd_t));
	INIT_LIST_HEAD(&vbd.images);
	INIT_LIST_HEAD(&vbd.pending_requests);
	td_image_t *image = tapdisk_image_allocate("blah", DISK_TYPE_VHD, TD_OPEN_RDONLY | TD_OPEN_SHAREABLE);
	list_add_tail(&image->next, &vbd.images);

	tapdisk_extents_t extents;
	bzero(&extents, sizeof(extents));
	
	td_vbd_request_t vreq;
	bzero(&vreq, sizeof(td_vbd_request_t));
	INIT_LIST_HEAD(&vreq.next);
	vreq.vbd = &vbd;
	vreq.iovcnt = 1;
	struct td_iovec iov;
	iov.base = 0;
	iov.secs = 123;
	vreq.iov = &iov;
	vreq.op = TD_OP_BLOCK_STATUS;
	vreq.data = &extents;

	td_request_t my_treq;
	bzero(&my_treq, sizeof(my_treq));
	my_treq.sidx           = 0;
	my_treq.buf            = iov.base;
	my_treq.sec            = vreq.sec;
	my_treq.secs           = iov.secs;
	my_treq.image          = image;
	my_treq.cb_data        = NULL;
	my_treq.vreq           = &vreq;
	my_treq.status	       = TD_BLOCK_STATE_HOLE;
	my_treq.op = TD_OP_BLOCK_STATUS;
	my_treq.cb = tapdisk_vbd_complete_block_status_request;

	tapdisk_vbd_complete_block_status_request(my_treq, 0);
	assert_non_null(extents.head);
	assert_int_equal(extents.head->flag, TD_BLOCK_STATE_HOLE);
	assert_int_equal(extents.head->start, my_treq.sec);
	assert_int_equal(extents.head->length, my_treq.secs);
}
