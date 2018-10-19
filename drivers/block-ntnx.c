/*
 * Copyright (C) 2016 Nutanix, Inc. All rights reserved.
 *
 * Author: Mike Cui <cui@nutanix.com>
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

#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <uuid/uuid.h>
#include <dlfcn.h>

#include "tapdisk.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-utils.h"
#include "tapdisk-fdreceiver.h"
#include "timeout-math.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "block-ntnx.h"

#define LIBFRODOISCSI_SO_PATH      "/opt/nutanix/lib/libfrodoiscsi.so"
#define LIBFRODOISCSI_SYMBOL       "frodo_iscsi_interface"
#define INITIATOR_PREFIX           "iqn.2016-06.com.nutanix:tapdisk-"
#define NTNX_ISCSI_PORTAL          "127.0.0.1:3260"
#define QUEUE_DEPTH TAPDISK_DATA_REQUESTS

struct tdntnx_request {
    td_request_t treq;
    td_driver_t *driver;
    struct tdntnx_request *next;
};

struct tdntnx_data {
    struct iscsi_session *session;
    struct tdntnx_request *freelist;
    struct tdntnx_request slots[QUEUE_DEPTH];
};
static const struct frodo_iscsi_interface *frodo;

static void
event_callback(event_id_t event_id, char mode, void *data)
{
    int64_t timeout_nsecs = frodo->schedule();

    tapdisk_server_event_set_timeout(
        event_id, timeout_nsecs < 0 ? TV_INF : TV_USECS(timeout_nsecs / 1000));
}

static int
block_ntnx_init(void)
{
    void *handle;
    int fd;
    uuid_t initiator_uuid;
    char initiator_name[sizeof INITIATOR_PREFIX + 36] = INITIATOR_PREFIX;

    handle = dlopen(LIBFRODOISCSI_SO_PATH, RTLD_LAZY);
    if (handle == NULL) {
        EPRINTF("dlopen '%s': %s\n", LIBFRODOISCSI_SO_PATH, dlerror());
        goto err;
    }

    frodo = dlsym(handle, LIBFRODOISCSI_SYMBOL);
    if (frodo == NULL) {
        EPRINTF("dlsym '%s': %s\n", LIBFRODOISCSI_SYMBOL, dlerror());
        goto err;
    }

    uuid_generate_random(initiator_uuid);
    uuid_unparse(initiator_uuid, initiator_name + strlen(initiator_name));

    fd = frodo->init(initiator_name, QUEUE_DEPTH);
    if (fd < 0) {
        EPRINTF("Failed to initialize libfrodoiscsi\n");
        goto err;
    }

    tapdisk_server_register_event(
        SCHEDULER_POLL_READ_FD | SCHEDULER_POLL_TIMEOUT,
        fd, TV_ZERO, event_callback, NULL);

    return 0;

 err:
    frodo = NULL;
    if (handle) {
        dlclose(handle);
    }
    return -1;
}

static struct tdntnx_request *
allocate_slot(struct tdntnx_data *prv)
{
    struct tdntnx_request *req;

    req = prv->freelist;
    if (req) {
        prv->freelist = req->next;
    }

    return req;
}

static void
free_slot(struct tdntnx_data *prv, struct tdntnx_request *req)
{
    req->next = prv->freelist;
    prv->freelist = req;
}

static int
iscsi_read_capacity_sync(struct iscsi_session *session, td_driver_t *driver)
{
    struct {
        uint64_t max_lba;
        uint32_t block_size;
        char padding[20];
    } result;
    const uint8_t cdb[16] = {
        [0] = 0x9e,           // SERVICE ACTION IN
        [1] = 0x10,           // READ CAPACITY (16)
        // 4 byte allocation size at offset 10.
        [10] = 0,
        [11] = 0,
        [12] = 0,
        [13] = sizeof result,
    };
    int status;

    status = frodo->sync_command(session, 0, cdb, &result, sizeof result, 0);
    if (status == 0) {
        driver->info.sector_size = ntohl(result.block_size);
        driver->info.size = ntohll(result.max_lba) + 1;
        driver->info.info = 0;
        return 0;
    }

    return -1;
}

static void
async_command_cb(void *ctx, int status, const uint8_t *sense, size_t sense_len)
{
    struct tdntnx_request *req = ctx;
    struct tdntnx_data *prv = req->driver->data;
    const uint8_t *const s = sense;

    switch (status) {
    case SCSI_STATUS_GOOD:
        status = 0;
        break;
    case SCSI_STATUS_CHECK_CONDITION:
        EPRINTF("scsi request failed: CHECK_CONDITION sense "
                "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9],
                s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17]);
        status = -EIO;
        break;
    case SCSI_STATUS_BUSY:
    case SCSI_STATUS_TASK_ABORTED:
        status = -EBUSY; // tapdisk will retry
        break;
    default:
        EPRINTF("scsi request failed: status %d\n", status);
        status = -EIO;
    }

    td_complete_request(req->treq, status);
    free_slot(prv, req);
}

static void
queue_io(td_driver_t* driver, const td_request_t *treq, int write)
{
    struct tdntnx_data *prv = driver->data;
    uint64_t lba     = treq->sec;
    uint32_t sectors = treq->secs;
    const uint8_t cdb[16] = {
        write ? 0x8A : 0x88, // WRITE/READ(16)
        0,
        // 8 byte LBA
        lba >> 56, lba >> 48, lba >> 40, lba >> 32,
        lba >> 24, lba >> 16, lba >> 8, lba,
        // 4 byte length
        sectors >> 24, sectors >> 16, sectors >> 8, sectors,
        0,
        0,
    };
    const size_t bytes = 1UL * sectors * driver->info.sector_size;
    struct tdntnx_request *req = allocate_slot(driver->data);
    int status;

    if (req) {
        req->treq = *treq;

        status = frodo->async_command(prv->session, 0, cdb,
                                      req->treq.buf, bytes, write,
                                      async_command_cb, req);
        if (status == 0) {
            return;
        }
        free_slot(prv, req);
    }

    td_complete_request(*treq, -EBUSY);
}

/* -- interface -- */

static int
tdntnx_open(td_driver_t* driver, const char* name,
	    struct td_vbd_encryption *encryption, td_flag_t flags)
{
    struct tdntnx_data *prv = driver->data;
    static int initialized = 0;
    int i;

    if (initialized == 0) {
        if (block_ntnx_init() < 0) {
            return -ENODEV;
        }
        initialized = 1;
    }

    DPRINTF("Creating iscsi session: %s %s\n", NTNX_ISCSI_PORTAL, name);
    prv->session = frodo->session_create(NTNX_ISCSI_PORTAL, name);
    if (!prv->session) {
        return -ENOENT;
    }
    if (iscsi_read_capacity_sync(prv->session, driver) != 0) {
        EPRINTF("Read capacity failed on target %s, closing session.\n", name);
        frodo->session_destroy(prv->session);
        return -ENOENT;
    }

    DPRINTF("Target %s has %ld sectors of %ld bytes\n",
            name, driver->info.size, driver->info.sector_size);

    prv->freelist = NULL;
    for (i = 0; i < QUEUE_DEPTH; i++) {
        prv->slots[i].driver = driver;
        free_slot(prv, &prv->slots[i]);
    }

    return 0;
}

static int
tdntnx_close(td_driver_t* driver)
{
    struct tdntnx_data *prv = driver->data;

    frodo->session_destroy(prv->session);

    return 0;
}

static void
tdntnx_queue_read(td_driver_t* driver, td_request_t treq)
{
    queue_io(driver, &treq, 0);
}

static void
tdntnx_queue_write(td_driver_t* driver, td_request_t treq)
{
    queue_io(driver, &treq, 1);
}

static int
tdntnx_get_parent_id(td_driver_t* driver, td_disk_id_t* id)
{
    return TD_NO_PARENT;
}

static int
tdntnx_validate_parent(td_driver_t *driver,
                       td_driver_t *parent, td_flag_t flags)
{
    return -EINVAL;
}

struct tap_disk tapdisk_ntnx = {
    .disk_type          = "tapdisk_ntnx",
    .private_data_size  = sizeof(struct tdntnx_data),
    .flags              = 0,
    .td_open            = tdntnx_open,
    .td_close           = tdntnx_close,
    .td_queue_read      = tdntnx_queue_read,
    .td_queue_write     = tdntnx_queue_write,
    .td_get_parent_id   = tdntnx_get_parent_id,
    .td_validate_parent = tdntnx_validate_parent,
};
