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
