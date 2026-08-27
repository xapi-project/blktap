/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TAPDISK_VALVE_H_
#define _TAPDISK_VALVE_H_

#define TD_VALVE_SOCKDIR          "/var/run/blktap/ratelimit"
#define TD_RLB_CONN_MAX           1024
#define TD_RLB_REQUEST_MAX        (8 << 20)

struct td_valve_req {
	unsigned long need;
	unsigned long done;
};

#endif /* _TAPDISK_VALVE_H_ */
