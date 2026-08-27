/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __TAPDISK_CONTROL_H__
#define __TAPDISK_CONTROL_H__

int tapdisk_control_open(char **path);
void tapdisk_control_close(void);

#endif
