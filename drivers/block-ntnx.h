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

/*
 * This file defines a struct of function pointers which defines a stable ABI
 * for tapdisk to use libfrodoiscsi. Tapdisk does not link directly against
 * this library. Instead, tapdisk dlopen()'s the .so at run time and looks for
 * a the symbol 'const struct frodo_icsi_interface', and then calls into
 * the libraries through the function pointers inside the struct.
 */

#ifndef __BLOCK_NTNX_H__
#define __BLOCK_NTNX_H__

#include <stdint.h>

struct iscsi_session;

enum {
    SCSI_STATUS_GOOD                 = 0x0,
    SCSI_STATUS_CHECK_CONDITION      = 0x02,
    SCSI_STATUS_BUSY                 = 0x08,
    SCSI_STATUS_RESERVATION_CONFLICT = 0x18,
    SCSI_STATUS_TASK_SET_FULL        = 0x28,
    SCSI_STATUS_ACA_ACTIVE           = 0x30,
    SCSI_STATUS_TASK_ABORTED         = 0x40,
};

/*
 * Completion callback for an iSCSI command.
 * Negative status indicates that the task failed at the iSCSI layer.
 * Non-negative status is the status of the SCSI command.
 * If status is 2 (CHECK_CONDITION), the SCSI sense buffer is provided.
 */
typedef void (*frodo_iscsi_complete_cb)(void *ctx, int status,
                                        const uint8_t *sense, size_t sense_len);

struct frodo_iscsi_interface {
    /* Initializes frodo iSCSI library, returns a file descriptor for polling. */
    int (*init)(const char *initiator_name, int queue_depth);

    /* Create iSCSI session. */
    struct iscsi_session *(*session_create)(const char *portal,
                                            const char *target);
    /* Destroy iSCSI session. */
    void (*session_destroy)(struct iscsi_session *session);

    /* Start asynchronous SCSI command, returns 0 if the command was submitted,
       otherwise returns -1 and complete_cb will not invoked. */
    int (*async_command)(struct iscsi_session *session, int lun,
                         const uint8_t *cdb, void *buf, size_t size, int write,
                         frodo_iscsi_complete_cb complete_cb, void *ctx);
    /* Run synchronous SCSI command, returns the status of the command. */
    int (*sync_command)(struct iscsi_session *session, int lun,
                        const uint8_t *cdb, void *buf, size_t size, int write);

    /* Schedule the iSCSI library to run. Returns the timeout in nanoseconds
       until this function should be called again or -1 for infinite timeout. */
    int64_t (*schedule)(void);
};

extern const struct frodo_iscsi_interface frodo_iscsi_interface;

#endif
