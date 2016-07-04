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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <endian.h>
#include <byteswap.h>

#include "partition.h"
#include "util.h"

#if BYTE_ORDER == LITTLE_ENDIAN
  #define le16_to_cpu(x) (x)
  #define le32_to_cpu(x) (x)
  #define cpu_to_le16(x) (x)
  #define cpu_to_le32(x) (x)
#else
  #define le16_to_cpu(x) bswap_16(x)
  #define le32_to_cpu(x) bswap_32(x)
  #define cpu_to_le16(x) bswap_16(x)
  #define cpu_to_le32(x) bswap_32(x)
#endif

void
primary_partition_in(struct primary_partition *p)
{
	p->lba    = le32_to_cpu(p->lba);
	p->blocks = le32_to_cpu(p->blocks);	
}

void
primary_partition_out(struct primary_partition *p)
{
	p->lba    = cpu_to_le32(p->lba);
	p->blocks = cpu_to_le32(p->blocks);	
}

void
partition_table_in(struct partition_table *pt)
{
	int i;

	pt->disk_signature = le32_to_cpu(pt->disk_signature);
	pt->mbr_signature  = le16_to_cpu(pt->mbr_signature);

	for (i = 0; i < ARRAY_SIZE(pt->partitions); i++)
		primary_partition_in(pt->partitions + i);
}

void
partition_table_out(struct partition_table *pt)
{
	int i;

	pt->disk_signature = cpu_to_le32(pt->disk_signature);
	pt->mbr_signature  = cpu_to_le16(pt->mbr_signature);

	for (i = 0; i < ARRAY_SIZE(pt->partitions); i++)
		primary_partition_out(pt->partitions + i);
}

int
primary_partition_validate(struct primary_partition *p)
{
	if (p->status != PARTITION_BOOTABLE &&
	    p->status != PARTITION_NON_BOOTABLE)
		return EINVAL;

	return 0;
}

int
partition_table_validate(struct partition_table *pt)
{
	int i;

	if (pt->mbr_signature != MBR_SIGNATURE)
		return EINVAL;

	for (i = 0; i < ARRAY_SIZE(pt->partitions); i++) {
		int err = primary_partition_validate(pt->partitions + i);
		if (err)
			return err;
	}

	return 0;
}

struct partition_chs
lba_to_chs(struct partition_geometry *geo, uint64_t lba)
{
	struct partition_chs c;

	if (lba >= 0x3ff * geo->sectors * geo->heads) {
		c.chs[0]  = geo->heads - 1;
		c.chs[1]  = geo->sectors;
		lba       = 0x3ff;
	} else {
		c.chs[1]  = lba % geo->sectors + 1;
		lba      /= geo->sectors;

		c.chs[0]  = lba % geo->heads;
		lba      /= geo->heads;
	}

	c.chs[2]  = lba & 0xff;
	c.chs[1] |= (lba >> 2) & 0xc0;

	return c;
}
