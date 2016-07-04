/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
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
 *
 * Commonly used headers and definitions related to blktap3.
 */

#ifndef __BLKTAP_3_H__
#define __BLKTAP_3_H__

#ifdef _BLKTAP
#include "compiler.h"
#endif

#define TAPBACK_CTL_SOCK_PATH       "/var/run/tapback.sock"
#define BLKTAP2_DEVNAME             "tapdev"

/**
 * Flag defines
 */
#define BT3_LOW_MEMORY_MODE 0x0000000000000001

/**
 * blkback-style stats
 */
struct blkback_stats {
	/**
	 * BLKIF_OP_DISCARD, not currently supported in blktap3, should always
	 * be zero
	 */
	unsigned long long st_ds_req;

	/**
	 * BLKIF_OP_FLUSH_DISKCACHE, not currently supported in blktap3,
	 * should always be zero
	 */
	unsigned long long st_f_req;

	/**
	 * Increased each time we fail to allocate memory for a internal
	 * request descriptor in response to a ring request.
	 */
	unsigned long long st_oo_req;

	/**
	 * Received BLKIF_OP_READ requests.
	 */
	unsigned long long st_rd_req;

	/**
	 * Completed BLKIF_OP_READ requests.
	 */
	long long st_rd_cnt;

	/**
	 * Read sectors, after we've forwarded the request to actual storage.
	 */
	unsigned long long st_rd_sect;

	/**
	 * Sum of the request response time of all BLKIF_OP_READ, in us.
	 */
	long long st_rd_sum_usecs;

	/**
	 * Absolute maximum BLKIF_OP_READ response time, in us.
	 */
	long long st_rd_max_usecs;

	/**
	 * Received BLKIF_OP_WRITE requests.
	 */
	unsigned long long st_wr_req;

	/**
	 * Completed BLKIF_OP_WRITE requests.
	 */
	long long st_wr_cnt;

	/**
	 * Write sectors, after we've forwarded the request to actual storage.
	 */
	unsigned long long st_wr_sect;

	/**
	 * Sum of the request response time of all BLKIF_OP_WRITE, in us.
	 */
	long long st_wr_sum_usecs;

	/**
	 * Absolute maximum BLKIF_OP_WRITE response time, in us.
	 */
	long long st_wr_max_usecs;

	/**
	 * Allocated space for 64 flags (due to 8-byte alignment)
	 * 1st flag is LSB, last flag is MSB.
	 * mem_mode: 0 - NORMAL_MEMORY_MODE; 1 - LOW_MEMORY_MODE;
	 */
	unsigned long long flags;
} __attribute__ ((aligned (8)));

#endif /* __BLKTAP_3_H__ */
