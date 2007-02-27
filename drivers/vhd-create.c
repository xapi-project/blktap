/* vhd-create.c
 *
 * Generates a vhd format disk.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <string.h>

#define TAPDISK
#include "tapdisk.h"

#if 1
#define DFPRINTF(_f, _a...) fprintf ( stderr, _f , ## _a )
#else
#define DFPRINTF(_f, _a...) ((void)0)
#endif

#define MAX_NAME_LEN 1000

void 
help(void)
{
	fprintf(stderr, "vhd-utils: v1.0.0\n");
	fprintf(stderr, 
		"usage: vhd-create [-h help] [-r reserve] <SIZE(MB)> <FILENAME> "
		"[<BACKING_FILENAME>]\n"); 
	exit(-1);
}

int
main(int argc, char **argv)
{
	uint64_t size;
	int ret = -1, c, sparse = 1;
	char filename[MAX_NAME_LEN], bfilename[MAX_NAME_LEN], *backing = NULL;
	struct td_state tds;
	struct disk_driver dd;

        for(;;) {
                c = getopt(argc, argv, "hr");
                if (c == -1)
                        break;
                switch(c) {
                case 'h':
                        help();
                        exit(0);
                        break;
                case 'r':
			sparse = 0;
			break;
		default:
			fprintf(stderr, "Unknown option\n");
			help();
		}
	}

	if (optind != (argc - 2) && optind != (argc - 3))
		help();

	size = atoi(argv[optind++]);
	size = size << 20;

	if (snprintf(filename, MAX_NAME_LEN, 
		     "%s", argv[optind++]) >= MAX_NAME_LEN) {
		fprintf(stderr,"Device name too long\n");
		exit(-1);
	}

	if (optind != argc) {
		/*Backing file argument*/
		if (!sparse) {
			fprintf(stderr, "Differencing disks must be sparse\n");
			exit(-1);
		}
		if (snprintf(bfilename, MAX_NAME_LEN, 
			     "%s", argv[optind++]) >= MAX_NAME_LEN) {
			fprintf(stderr, "Device name too long\n");
			exit(-1);
		}
		backing = bfilename;
	}

	DFPRINTF("Creating file size %llu, name %s\n",
		 (long long unsigned)size, filename);
	
	dd.drv      = dtypes[DISK_TYPE_VHD]->drv;
	dd.td_state = &tds;
	dd.private  = malloc(dd.drv->private_data_size);
	if (!dd.private) {
		fprintf(stderr, "Error: no memory\n");
		exit(-1);
	}

	ret = vhd_create(&dd, filename, size, backing, sparse);
	if (ret < 0) 
		fprintf(stderr, "Unable to create VHD file\n");

	return 0;
}
