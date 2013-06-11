/* 
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
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
