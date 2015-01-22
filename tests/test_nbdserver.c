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

