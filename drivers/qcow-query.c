/* qcow-create.c
 *
 * Queries a qcow format disk.
 *
 * (c) 2006 Andrew Warfield and Julian Chesterfield
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
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
#include "bswap.h"
#include "tapdisk.h"

#define MAX_NAME_LEN 1000

void help(void)
{
	fprintf(stderr, "Qcow-utils: v1.0.0\n");
	fprintf(stderr, 
		"usage: qcow-query [-h help] [-v virtsize] <FILENAME>\n"); 
	exit(-1);
}

int query_size(struct disk_driver *dd)
{	
	struct td_state *bs = dd->td_state;

	return bs->size >> 11;
}

char *query_parent(struct disk_driver *dd)
{
	struct disk_id *id = malloc(sizeof(struct disk_id));
	int ret;

	ret = dd->drv->td_get_parent_id(dd, id);
	if (ret == TD_NO_PARENT || ret != 0) {
		return NULL;
	}
	return id->name;
}

int main(int argc, char *argv[])
{
	int ret = -1, c;
	int size = 0, parent = 0;
	char filename[MAX_NAME_LEN], *ptr = NULL;
	struct disk_driver ddqcow;


	ddqcow.td_state = malloc(sizeof(struct td_state));

        for(;;) {
                c = getopt(argc, argv, "hvp");
                if (c == -1)
                        break;
                switch(c) {
                case 'h':
                        help();
                        exit(0);
                        break;
                case 'v':
		  size = 1;
			break;
                case 'p':
		  parent = 1;
			break;
		default:
			fprintf(stderr, "Unknown option\n");
			help();
		}
	}

	if (snprintf(filename, MAX_NAME_LEN, "%s",argv[optind++]) >=
	    MAX_NAME_LEN) {
	  fprintf(stderr,"Device name too long\n");
	  exit(-1);
        }

	ddqcow.drv = &tapdisk_qcow;
	ddqcow.private = malloc(ddqcow.drv->private_data_size);

        if (ddqcow.drv->td_open(&ddqcow, filename, TD_RDONLY)!=0) {
		fprintf(stderr, "Unable to open Qcow file [%s]\n",filename);
		exit(-1);
	} 

	if (size) 
		printf("%d\n",query_size(&ddqcow));

	if (parent) {
		ptr = query_parent(&ddqcow);
		if (!ptr) {
			printf("Query failed\n");
			return EINVAL;
		}
		printf("%s\n",ptr);
	}

	return 0;
}
