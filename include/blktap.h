/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TD_BLKTAP_H_
#define _TD_BLKTAP_H_

#define BLKTAP2_CONTROL_NAME           "blktap/control"
#define BLKTAP2_CONTROL_DIR            "/run/blktap-control"
#define BLKTAP2_NP_RUN_DIR             BLKTAP2_CONTROL_DIR"/tapdisk"
#define BLKTAP2_CONTROL_SOCKET         "ctl"
#define BLKTAP2_ENOSPC_SIGNAL_FILE     "/run/tapdisk-enospc"

/* Maximum number of possible minor ids, to match old kernel definition */
#define MAX_ID  16384

#endif /* _TD_BLKTAP_H_ */
