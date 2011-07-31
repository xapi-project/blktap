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
