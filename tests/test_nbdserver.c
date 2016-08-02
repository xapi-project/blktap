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

#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif /*_LARGEFILE64_SOURCE */
#include <sys/types.h>

#include "unity.h"

#include <string.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <stdlib.h>

#include "drivers/tapdisk-nbdserver.h"

#include "mock_tapdisk-log.h"
#include "mock_tapdisk-server.h"
#include "mock_tapdisk-utils.h"
#include "mock_tapdisk-vbd.h"
#include "mock_tapdisk-fdreceiver.h"

unsigned PAGE_SIZE = 1 << 12;

td_nbdserver_t server;
td_nbdserver_client_t *client;

void setUp(void) {

	tlog_syslog_Ignore();
	tapdisk_server_unregister_event_Ignore();

	memset(&server, 0, sizeof server);
	INIT_LIST_HEAD(&server.clients);

	/*
	 * XXX We leak client as it is complicated to deallocate it in tearDown():
	 * whether client is deallocated by a test depends on the test itself and
	 * whether the test succeeds or fails.
	 */
	client = tapdisk_nbdserver_alloc_client(&server);
	assert(client);

}

void tearDown(void) {
}

void test_nbdserver_client(void) {

	TEST_ASSERT_TRUE_MESSAGE(
			tapdisk_nbdserver_contains_client(&server, client),
			"A just-allocated client should be present in the server's list "
			"of clients");

	TEST_ASSERT_FALSE_MESSAGE(client->dead,
			"A just-allocated client should not be marked dead.");
}

void test_nbdserver_client_free(void) {

	tapdisk_nbdserver_free_client(client);

	TEST_ASSERT_FALSE_MESSAGE(
			tapdisk_nbdserver_contains_client(&server, client),
			"Freeing a client with no pending requests should result in the "
			"client being immediately freed.");
}

void test_nbdserver_client_pending_req(void)
{
	td_nbdserver_req_t *req;

	req = tapdisk_nbdserver_alloc_request(client);
	assert(req);

	tapdisk_nbdserver_clientcb(0, 0, client);

	TEST_ASSERT_TRUE_MESSAGE(
			tapdisk_nbdserver_contains_client(&server, client),
			"NBD client shouldn't be freed while there are pending requests");
	TEST_ASSERT_TRUE_MESSAGE(client->dead, "NBD client should be marked dead");

	tapdisk_nbdserver_free_request(client, req);

	TEST_ASSERT_TRUE_MESSAGE(
		tapdisk_nbdserver_contains_client(&server, client),
		"NBD client marked dead should be freed when last request completes");
}

