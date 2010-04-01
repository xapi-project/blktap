#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <endian.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <byteswap.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/hdreg.h>

#include "partition.h"

#if BYTE_ORDER == LITTLE_ENDIAN
  #define cpu_to_le32(x) (x)
  #define cpu_to_le64(x) (x)
#else
  #define cpu_to_le32(x) bswap_32(x)
  #define cpu_to_le64(x) bswap_64(x)
#endif

static void
usage(const char *app)
{
	printf("usage: %s <-i image> "
	       "[-d dump] [-c count] [-f format] "
	       "[-t type] [-s sig <part>]\n", app);
}

static void
chs_unpack(struct partition_chs *c,
	   uint8_t *head, uint8_t *sector, uint16_t *cylinder)
{
	*head = c->chs[0];
	*sector = c->chs[1] & 0x3f;
	*cylinder = (c->chs[1] & 0xc0) * 4 + c->chs[2];
}

void
partition_table_dump(struct partition_table *pt)
{
	int i;

	printf("disk signature   0x%08x\n", pt->disk_signature);
	printf("mbr signature    0x%04x\n", pt->mbr_signature);
	printf("\n");

	for (i = 0; i < 4; i++) {
		struct primary_partition *p = pt->partitions + i;
		uint8_t head, sector;
		uint16_t cylinder;

		printf("  %d status       0x%02x\n", i, p->status);

		chs_unpack(&p->chs_first, &head, &sector, &cylinder);
		printf("  %d s cylinder   0x%04x\n", i, cylinder);
		printf("  %d s sector     0x%01x\n", i, sector);
		printf("  %d s head       0x%01x\n", i, head);

		printf("  %d type         0x%01x\n", i, p->type);

		chs_unpack(&p->chs_last, &head, &sector, &cylinder);
		printf("  %d e cylinder   0x%04x\n", i, cylinder);
		printf("  %d e sector     0x%01x\n", i, sector);
		printf("  %d e head       0x%01x\n", i, head);

		printf("  %d lba          0x%08x\n", i, p->lba);
		printf("  %d blocks       0x%08x\n", i, p->blocks);

		printf("\n");
	}
}

static int
dump_partitions(const char *image)
{
	int fd, ret;
	struct partition_table pt;

	ret = 1;
	fd  = -1;

	fd = open(image, O_RDONLY);
	if (fd == -1)
		goto out;

	if (read(fd, &pt, sizeof(pt)) != sizeof(pt)) {
		errno = errno ? : EIO;
		goto out;
	}

	partition_table_in(&pt);
	if (partition_table_validate(&pt)) {
		errno = EINVAL;
		printf("table invalid\n");
		goto out;
	}

	partition_table_dump(&pt);
	ret = 0;

out:
	close(fd);
	return ret;
}

static void
__dump_signature(struct partition_table *pt, int part)
{
	if (part < 1 || part > 4)
		errno = EINVAL;
	else {
		uint8_t *p, *s;
		uint32_t sig = pt->disk_signature;
		uint64_t off = (uint64_t)pt->partitions[part - 1].lba << 9;

		sig = cpu_to_le32(sig);
		off = cpu_to_le64(off);

		for (p = s = (uint8_t *)&sig; p - s < sizeof(sig); p++)
			printf("%02x", *p);

		for (p = s = (uint8_t *)&off; p - s < sizeof(off); p++)
			printf("%02x", *p);

		printf("\n");
	}
}

static int
dump_signature(const char *image, int part)
{
	int fd, ret;
	struct partition_table pt;

	ret = 1;
	fd  = -1;

	fd = open(image, O_RDONLY);
	if (fd == -1)
		goto out;

	if (read(fd, &pt, sizeof(pt)) != sizeof(pt)) {
		errno = errno ? : EIO;
		goto out;
	}

	partition_table_in(&pt);
	if (partition_table_validate(&pt)) {
		errno = EINVAL;
		printf("table invalid\n");
		goto out;
	}

	__dump_signature(&pt, part);
	ret = 0;

out:
	close(fd);
	return ret;
}

static int
count_partitions(const char *image, int *count)
{
	int i, fd, ret;
	struct partition_table pt;

	ret = 1;
	fd  = -1;

	fd = open(image, O_RDONLY);
	if (fd == -1)
		goto out;

	if (read(fd, &pt, sizeof(pt)) != sizeof(pt)) {
		errno = errno ? : EIO;
		goto out;
	}

	partition_table_in(&pt);
	if (partition_table_validate(&pt)) {
		*count = 0;
		goto done;
	}

	*count = 0;
	for (i = 0; i < 4; i++)
		if (pt.partitions[i].type)
			(*count)++;

done:
	ret = 0;
out:
	close(fd);
	return ret;
}

static int
format_partition(const char *image, int type, struct partition_table *pt)
{
	uint64_t lend;
	uint32_t start, end;
	int ret, sec_size, fd;
	unsigned int cylinders;
	struct hd_geometry geo;
	struct primary_partition *pp;
	struct partition_geometry pgeo;
	unsigned long long bytes, llcyls;

	ret = 1;
	fd  = -1;

	memset(pt, 0, sizeof(*pt));
	pp = pt->partitions;

	srandom(time(NULL));

	fd = open(image, O_RDWR);
	if (fd == -1)
		goto out;

	if (ioctl(fd, HDIO_GETGEO, &geo))
		goto out;

	if (ioctl(fd, BLKGETSIZE64, &bytes))
		goto out;

	if (ioctl(fd, BLKSSZGET, &sec_size))
		goto out;

	llcyls = (bytes >> 9) / ((sec_size >> 9) * geo.heads * geo.sectors);
	cylinders = llcyls;
	if (cylinders != llcyls)
		cylinders = ~0;

	pgeo.heads          = geo.heads;
	pgeo.sectors        = geo.sectors;
	pgeo.cylinders      = cylinders;

	start               = pgeo.sectors;
	lend                = geo.heads * geo.sectors * llcyls - 1;

	end = lend;
	if (end != lend)
		end = ~0;

	pp->status          = PARTITION_BOOTABLE;
	pp->type            = type;
	pp->lba             = start;
	pp->blocks          = end - start + 1;
	pp->chs_first       = lba_to_chs(&pgeo, start);
	pp->chs_last        = lba_to_chs(&pgeo, lend);

	pt->mbr_signature   = MBR_SIGNATURE;
	pt->disk_signature  = random();

	partition_table_out(pt);
	if (write(fd, pt, sizeof(*pt)) != sizeof(*pt)) {
		errno = errno ? : EIO;
		goto out;
	}

	ret = 0;

out:
	close(fd);
	return ret;
}

int
main(int argc, char *argv[])
{
	char *image;
	struct partition_table pt;
	int ret, c, type, count, dump, format, signature;

	ret       = 1;
	format    = 0;
	count     = 0;
	dump      = 0;
	type      = 0;
	signature = -1;
	image     = NULL;

	while ((c = getopt(argc, argv, "i:fdt:cs:h")) != -1) {
		switch (c) {
		case 'i':
			image = optarg;
			break;
		case 'c':
			count = 1;
			break;
		case 's':
			signature = atoi(optarg);
			break;
		case 'f':
			format = 1;
			break;
		case 't': {
			int base = (!strncasecmp(optarg, "0x", 2) ? 16 : 10);
			type = strtol(optarg, NULL, base);
			break;
		}
		case 'd':
			dump = 1;
			break;
		case 'h':
			usage(argv[0]);
			ret = 0;
			goto out;
		}
	}

	if (!image || (!format && !count && !signature && !dump)) {
		errno = EINVAL;
		usage(argv[0]);
		goto out;
	}

	if (format) {
		if (!type) {
			errno = EINVAL;
			perror("type required");
			goto out;
		}

		if (format_partition(image, type, &pt)) {
			perror("formatting partition");
			goto out;
		}

		__dump_signature(&pt, 1);
	}

	if (count) {
		if (count_partitions(image, &count)) {
			perror("counting partitions");
			goto out;
		}
		printf("%d\n", count);
	}

	if (signature != -1) {
		if (dump_signature(image, signature)) {
			perror("dumping signature");
			goto out;
		}
	}

	if (dump) {
		if (dump_partitions(image)) {
			perror("dumping partitions");
			goto out;
		}
	}

	ret = 0;

out:
	return ret;
}
