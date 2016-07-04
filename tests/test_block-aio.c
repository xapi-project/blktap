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

#include "unity.h"
#include <stdlib.h>

/* Header file for SUT */
#include "drivers/block-aio.h"

/* Mocks */
#include "mock_tapdisk-interface.h"
#include "mock_tapdisk-stats.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_tdaio_queue_read_computes_size_and_offset_correctly(void)
{
    // Initialisation
    td_driver_t driver;
    td_request_t treq;

    int expected_size;
    uint64_t expected_offset;
    struct aio_request aio;
    struct tdaio_state prv;

    driver.data = &prv;
    treq.secs = 10;
    driver.info.sector_size = 2048;
    treq.sec = (uint64_t) 23;

    prv.aio_free_count = 1;

    prv.aio_free_list[0] = &aio;

    // Expectations
    expected_size = treq.secs * SECTOR_SIZE;
    expected_offset = treq.sec * (uint64_t) SECTOR_SIZE;

    td_prep_read_Expect(
        &aio.tiocb,
        prv.fd,
        treq.buf,
        expected_size,
        expected_offset,
        tdaio_complete,
        &aio);

    td_queue_tiocb_Ignore();

    // Call to the method to test
    tdaio_queue_read(&driver, treq);
}
