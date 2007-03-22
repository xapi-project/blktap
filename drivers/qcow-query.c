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

#define MAX_NAME_LEN 1000

typedef struct QCowHeader {
        uint32_t magic;
        uint32_t version;
        uint64_t backing_file_offset;
        uint32_t backing_file_size;
        uint32_t mtime;
        uint64_t size; /* in bytes */
        uint8_t cluster_bits;
        uint8_t l2_bits;
        uint32_t crypt_method;
        uint64_t l1_table_offset;
} QCowHeader;


void help(void)
{
	fprintf(stderr, "Qcow-utils: v1.0.0\n");
	fprintf(stderr, 
		"usage: qcow-query [-h help] [-v virtsize] <FILENAME>\n"); 
	exit(-1);
}

int query_size(char *filename)
{
  int fd,len;
  QCowHeader header;

  fd = open(filename, O_RDWR | O_LARGEFILE);
  if (fd < 0) {
    fprintf(stderr,"Unable to open %s (%d)\n",filename,0 - errno);
    exit(-1);
  }

  if ((len = read(fd, &header, sizeof(QCowHeader))) != sizeof(QCowHeader)) {
    fprintf(stderr,"Failed to read %d bytes (%d)\n",sizeof(QCowHeader),len);
    exit(-1);
  }

  close(fd);

  be64_to_cpus(&header.size);
  return header.size/(1024 * 1024);
}

int main(int argc, char *argv[])
{
	int ret = -1, c;
	int size = 0;
	char filename[MAX_NAME_LEN];

        for(;;) {
                c = getopt(argc, argv, "hv");
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
	if (size) 
	  printf("%d\n",query_size(filename));

	return 0;
}
