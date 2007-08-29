#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define TAPDISK
#include "tapdisk.h"
#include "vhd.h"

#define BM_SIZE  (512)
#define BLK_SIZE (2 << 20)

#define nsize     15
static char nbuf[nsize];

static inline int
test_bit (int nr, volatile u32 *addr)
{
	return (((u32 *)addr)[nr >> 5] >> (nr & 31)) & 1;
}

static inline char *
__xconv(uint64_t num)
{
	snprintf(nbuf, nsize, "%#llx", num);
	return nbuf;
}

static inline char *
__dconv(uint64_t num)
{
	snprintf(nbuf, nsize, "%llu", num);
	return nbuf;
}

#define conv(hex, num) \
	(hex ? __xconv((uint64_t)num) : __dconv((uint64_t)num))

static int
vhd_print(struct disk_driver *dd, struct vhd_info *info, 
	  uint64_t block, int count, int bat, int bitmap, 
	  int data, int test, int hex)
{
	int i, fd, err;
	char *mbuf, *dbuf;
	uint64_t offset, cur;

	fd   = -1;
	err  = 0;
	mbuf = NULL;
	dbuf = NULL;

	if (bitmap || test != -1 || data) {
		if ((fd = open(dd->name, O_RDONLY | O_LARGEFILE)) == -1) {
			fprintf(stderr, "failed to open %s\n", dd->name);
			return errno;
		}

		if (bitmap || test != -1) {
			mbuf = malloc(BM_SIZE);
			if (!mbuf) {
				err = -ENOMEM;
				fprintf(stderr, "failed to alloc bitmap buffer\n");
				goto out;
			}
		}

		if (data) {
			dbuf = malloc(BLK_SIZE);
			if (!dbuf) {
				err = -ENOMEM;
				fprintf(stderr, "failed to alloc data buffer\n");
				goto out;
			}
		}
	}

	for (i = 0; i < count; i++) {
		cur = block + i;

		if (cur >= info->bat_entries) {
			fprintf(stderr, "invalid block %s: ", conv(hex, cur));
			fprintf(stderr, "this file only has %s blocks\n",
				conv(hex, info->bat_entries));
			return -ERANGE;
		}

		offset = info->bat[cur];
		if (offset == DD_BLK_UNUSED) {
			printf("block %s not allocated\n", conv(hex, cur));
			continue;
		}

		if (bat) {
			printf("block: %s: ", conv(hex, cur));
			printf("offset: %s\n", conv(hex, offset << SECTOR_SHIFT));
		}

		if (!bitmap && test == -1 && !data)
			continue;
	
		if (bitmap || test != -1) {
			off64_t off = offset << SECTOR_SHIFT;
			
			if (lseek64(fd, off, SEEK_SET) == (off64_t)-1) {
				err = errno;
				fprintf(stderr, "failed to seek to bitmap of "
					"block %s\n", conv(hex, cur));
				goto out;
			}

			if (read(fd, mbuf, BM_SIZE) != BM_SIZE) {
				err = -EIO;
				fprintf(stderr, "failed to read bitmap for "
					"block %s\n", conv(hex, cur));
				goto out;
			}

			if (test != -1) {
				int j;
				uint32_t sec;
				for (j = 0; j < count; j++) {
					sec = test + j;
					printf("block %s: ", conv(hex, cur));
					if (sec >= info->spb)
						printf("sec %s past end of block "
						       "(spb = %d)\n", 
						       conv(hex, sec), info->spb);
					else
						printf("sec %s: %d\n", conv(hex, sec),
						       test_bit(sec, (void *)mbuf));
				}
				goto out;
			}

			if (bitmap)
				write(STDOUT_FILENO, mbuf, BM_SIZE);
		}

		if (data) {
			int ret;
			off64_t off = (offset << SECTOR_SHIFT) + BM_SIZE;

			if (lseek64(fd, off, SEEK_SET) == (off64_t)-1) {
				err = errno;
				fprintf(stderr, "failed to seek to data of "
					"block %s\n", conv(hex, cur));
				goto out;
			}

			if ((ret = read(fd, dbuf, BLK_SIZE)) != BLK_SIZE) {
				err = -EIO;
				fprintf(stderr, "failed to read block "
					"%s, ret: %d\n", conv(hex, cur), ret);
				goto out;
			}

			write(STDOUT_FILENO, dbuf, BLK_SIZE);
		}
	}

 out:
	free(mbuf);
	free(dbuf);
	close(fd);
	return err;
}

int
vhd_read(struct disk_driver *dd, int argc, char *argv[])
{
	struct vhd_info info;
	uint64_t block, count;
	int c, err, bat, bitmap, data, num, hex, test;

	err     = 0;
	bat     = 0;
	bitmap  = 0;
	data    = 0;
	num     = 0;
	hex     = 0;
	count   = 1;
	test    = -1;
	block   = -1;

	while ((c = getopt(argc, argv, "a:bmdc:nxt:h")) != -1) {
		switch(c) {
		case 'a':
			block = strtoul(optarg, NULL, 10);
			break;
		case 'b':
			bat = 1;
			break;
		case 'm':
			bitmap = 1;
			break;
		case 'd':
			data = 1;
			break;
		case 'x':
			hex = 1;
			break;
		case 'c':
			count = strtoul(optarg, NULL, 10);
			break;
		case 'n':
			num = 1;
			break;
		case 't':
			test = strtoul(optarg, NULL, 10);
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
		case 'h':
			goto usage;
		}
	}

	if (optind != (argc - 1) || block == -1) {
		printf("optind: %d, argc: %d, block: %llu\n",
		       optind, argc, block);
		goto usage;
	}

	if (!bitmap && !data && !num && test == -1)
		bat = 1;

	err = vhd_get_info(dd, &info);
	if (err) {
		printf("Failed to get info for %s\n", dd->name);
		goto out;
	}

	if (num) {
		if (hex)
			printf("%s has %x blocks\n", dd->name, info.bat_entries);
		else
			printf("%s has %u blocks\n", dd->name, info.bat_entries);
	}

	err = vhd_print(dd, &info, block, count, bat, bitmap, data, test, hex);

 out:
	free(info.bat);
	return err;

 usage:
	fprintf(stderr, "usage: vhd read filename -a block [options]\n"
		"options:\n"
		"-h          help\n"
		"-b          print bat entry\n"
		"-m          print bitmap\n"
		"-d          print data\n"
		"-c num      print num blocks\n"
		"-x          print in hex\n"
		"-n          print the number of blocks in the vhd\n"
		"-t sec      test bitmap for sector offset of block\n");
	return EINVAL;
}
