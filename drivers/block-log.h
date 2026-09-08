/*
 * Copyright (c) 2017, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __BLOCK_LOG_H__
#define __BLOCK_LOG_H__

#include "cbt-util.h"

struct tdlog_data {
	uint64_t   	size;
	void*		bitmap;
};

#endif
