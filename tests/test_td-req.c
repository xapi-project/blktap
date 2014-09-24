#include "unity.h"
#include "drivers/tapdisk.h"
#include "mock_tapdisk-stats.h"
#include "mock_tapdisk-interface.h"
#include <xenctrl.h>
#include "drivers/td-req.h"
#include "drivers/tapdisk-utils.h"
#include "mock_td-ctx.h"
#include "mock_td-blkif.h"
#include "mock_tapdisk-server.h"
#include "mock_tapdisk-driver.h"
#include "mock_tapdisk-log.h"
#include "mock_tapdisk-vbd.h"
#include "mock_tapdisk-image.h"

unsigned int PAGE_SHIFT;


void setUp(void)
{
    tapdisk_server_mem_mode_IgnoreAndReturn(NORMAL_MEMORY_MODE);
}

void tearDown(void)
{
}

#define RING_SIZE 20

struct td_xenblkif* create_dead_blkif(void) {
    struct td_xenblkif* blkif;
    blkif_request_t* free_requests;

    blkif = malloc(sizeof(struct td_xenblkif));
    free_requests = malloc(RING_SIZE * sizeof(blkif_request_t));

    blkif->reqs_bufcache = malloc(RING_SIZE * sizeof(void*));
    blkif->n_reqs_bufcache_free = 0;

    blkif->dead = 1;
    blkif->n_reqs_free = 10;
    blkif->ring_size = RING_SIZE;
    blkif->reqs_free = free_requests;

    return blkif;
}


void test_comptetion_of_non_last_req_on_dead_ring_does_not_destroy_ring(void)
{
    struct td_xenblkif_req request;
    struct td_xenblkif* blkif;

    blkif = create_dead_blkif();

    /* We report that we still have pending requests */
    tapdisk_xenblkif_reqs_pending_IgnoreAndReturn(1);

    /* Note that we do not expect any call to tapdisk_xenblkif_destroy */

    /* We call complete request */
    tapdisk_xenblkif_complete_request(blkif, &request, 0, 0);

    /* At this point the framework verifies that all the calls happened */
}


void test_completion_of_last_req_on_dead_ring_destroys_ring(void)
{
    struct td_xenblkif_req request;
    struct td_xenblkif* blkif;

    blkif = create_dead_blkif();

    /* We report that this is the last request */
    tapdisk_xenblkif_reqs_pending_IgnoreAndReturn(0);

    /* We expect a call to tapdisk_xenblkif_destroy with parameter blkif */
    tapdisk_xenblkif_destroy_ExpectAndReturn(blkif, 0);

    /* We call complete request */
    tapdisk_xenblkif_complete_request(blkif, &request, 0, 0);

    /* At this point the framework verifies that all the calls happened */
}
