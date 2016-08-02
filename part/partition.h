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
 */

#ifndef _PARTITION_H_
#define _PARTITION_H_

#include <inttypes.h>

#define PARTITION_BOOTABLE            0x80
#define PARTITION_NON_BOOTABLE        0x00

#define MBR_SIGNATURE                 0xAA55
#define MBR_START_SECTOR              0x80

struct partition_geometry {
	unsigned char                 heads;
	unsigned char                 sectors;
	unsigned int                  cylinders;
};

struct partition_chs {
	uint8_t                       chs[3];
} __attribute__((__packed__));

struct primary_partition {
	uint8_t                       status;
	struct partition_chs          chs_first;
	uint8_t                       type;
	struct partition_chs          chs_last;
	uint32_t                      lba;
	uint32_t                      blocks;
} __attribute__((__packed__));

struct partition_table {
	uint8_t                       code[0x1b8];
	uint32_t                      disk_signature;
	uint8_t                       pad[0x2];
	struct primary_partition      partitions[4];
	uint16_t                      mbr_signature;
} __attribute__((__packed__));

void partition_table_in(struct partition_table *);
void partition_table_out(struct partition_table *);
int partition_table_validate(struct partition_table *);
void partition_table_dump(struct partition_table *);
struct partition_chs lba_to_chs(struct partition_geometry *, uint64_t);

#endif
