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
#include "tapdisk-nbdserver.h"
#include "tapdisk-protocol-new.h"

void
test_nbdserver_new_protocol_handshake(void **state)
{
	td_nbdserver_client_t client;
	td_nbdserver_t server;
	int new_fd =123 ;

	uint16_t gflags = (NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);
	struct nbd_new_handshake handshake;
	bzero(&handshake, sizeof(handshake));
	handshake.nbdmagic = htobe64 (NBD_MAGIC);
	handshake.version = htobe64 (NBD_OPT_MAGIC);
	handshake.gflags = htobe16 (gflags);

	/* Check input to send */
	expect_memory(__wrap_send, buf, &handshake, sizeof(handshake));
	expect_value(__wrap_send, fd, new_fd);
	expect_value(__wrap_send, size, sizeof(handshake));
	expect_value(__wrap_send, flags, 0);
	will_return(__wrap_send, sizeof(handshake));

	client.server = &server;

	expect_value(__wrap_tapdisk_server_register_event, cb, tapdisk_nbdserver_handshake_cb);
	expect_value(__wrap_tapdisk_server_register_event, mode, SCHEDULER_POLL_READ_FD);
	int err = tapdisk_nbdserver_new_protocol_handshake(&client, new_fd);
	assert_int_equal(err, 0);
}
