/*
 * Copyright (c) 2017, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CBT_UTIL_H_
#define _CBT_UTIL_H_

#include <uuid/uuid.h>
#include "../drivers/tapdisk-log.h"

#define CBT_BLOCK_SIZE (64 * 1024)

struct cbt_log_metadata {
	uuid_t 		parent;
	uuid_t 		child;
	int			consistent;
	uint64_t	size;
};

struct cbt_log_data {
	struct cbt_log_metadata metadata;
	char 					*bitmap;
};

static inline uint64_t
roundup_div(uint64_t a, int b)
{
	return (a + b - 1) / b;
}

static inline uint64_t 
bitmap_size(uint64_t sz)
{
	// Original disk size is in bytes
	uint64_t num_blocks = roundup_div(sz, CBT_BLOCK_SIZE);
	return (roundup_div(num_blocks, 8));
}


#endif
