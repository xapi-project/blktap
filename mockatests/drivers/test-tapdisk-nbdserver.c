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

/* tapdisk_vsyslog() will call down to syslog() and send().  That throws using
 * __wrap_send to check the handshake.  Interposing tapdisk_vsyslog side-steps
 * that problem. */
int
__wrap_tapdisk_vsyslog(void *log, int prio, const char *fmt, va_list ap)
{
	(void)log;
	(void)prio;
	(void)fmt;
	(void)ap;
	return mock();//return wrap_vprintf(fmt, ap);
}

void
test_nbdserver_newclient_fd_new_fixed(void **state)
{
	td_nbdserver_t server;
	int new_fd =123 ;

	/* Avoid segfault when tapdisk_nbdserver_alloc_client tries to add
	 * to list. */
	INIT_LIST_HEAD(&server.clients);

	uint16_t gflags = (NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);
	struct nbd_new_handshake handshake;
	bzero(&handshake, sizeof(handshake));
	handshake.nbdmagic = htobe64 (NBD_MAGIC);
	handshake.version = htobe64 (NBD_NEW_VERSION);
	handshake.gflags = htobe16 (gflags);

	will_return(__wrap_tapdisk_vsyslog, 0);

	/* Check input to send */
	expect_memory(__wrap_send, buf, &handshake, sizeof(handshake));
	expect_value(__wrap_send, fd, new_fd);
	expect_value(__wrap_send, size, sizeof(handshake));
	expect_value(__wrap_send, flags, 0);
	will_return(__wrap_send, sizeof(handshake));

	will_return(__wrap_tapdisk_vsyslog, 0);

	will_return(__wrap_tapdisk_vsyslog, 0);

	will_return(__wrap_tapdisk_vsyslog, 0);

	will_return(__wrap_tapdisk_vsyslog, 0);

	expect_value(__wrap_tapdisk_server_register_event, cb, tapdisk_nbdserver_handshake_cb);
	expect_value(__wrap_tapdisk_server_register_event, mode, SCHEDULER_POLL_READ_FD);

	tapdisk_nbdserver_newclient_fd_new_fixed(&server, new_fd);
}
