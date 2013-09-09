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

#ifndef _BLKTAP_2_H_
#define _BLKTAP_2_H_

#define BLKTAP2_CONTROL_NAME           "blktap-control"
#define BLKTAP2_CONTROL_DIR            "/var/run/"BLKTAP2_CONTROL_NAME
#define BLKTAP2_CONTROL_SOCKET         "ctl"
#define BLKTAP2_CONTROL_DEVICE         BLKTAP2_DIRECTORY"/control"
#define BLKTAP2_ENOSPC_SIGNAL_FILE     "/var/run/tapdisk-enospc"

#endif
