/* dcopy.c
 *
 * Direct copy a file, avoiding buffer caches and preserving sparseness.
 *
 *
 * Usage:
 * 
 * dcopy [-sparse] [-chunksize N] <src> <dest>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <err.h>
#include <inttypes.h>
#include "atomicio.h"

#define SECT_SIZE 512
#define PAGE_SIZE 4096

static char zero_sect[PAGE_SIZE];

static int verbose = 0;
#define VPRINTF(_v, _a...) if (_v < verbose) printf(_a)

void dcopy(int src, int dst, int sparse, int cs)
{
        struct stat stat;
        char *buf;
        int res;
        long i;
	off_t pos = 0;
        int dst_is_file = 1;

        /* If we are writing to a block device, we won't truncate later. */
        res = fstat(dst, &stat);
        if (res != 0)
                err(1, "stat()'ing destination file.");
        if (S_ISBLK(stat.st_mode))
                dst_is_file = 0;

        res = posix_memalign((void **)&buf, 4096, cs);
        if ( res != 0 )
        {
                errno = res;
                err(1, "allocating copy buffer. (of %d bytes)\n", cs);
        }

        while(1) 
        {
                long offset = 0;
                long start = 0;

                i = atomicio(read, src, buf, cs);
                if (i == -1)
                        err(1, "Reading from source file.");
                if (i == 0)
                        break;
                
                VPRINTF(2, "Read %ld bytes.\n", i);

                start = offset = 0;
                while (1) 
                {
                        start = offset;
                        VPRINTF(2, "(o: %ld, i: %ld)\n", offset, i); 
                        if ( offset >= i ) break;

                        /* Non-sparse region */
                        while ( ((!sparse) || 
                                 (memcmp(&buf[offset], zero_sect, 
                                         PAGE_SIZE) != 0 )) 
                                && (offset < i) )
                                offset += PAGE_SIZE;
                        if (offset > i) 
                                offset = i;
                        if ((offset - start) > 0)
                        {
                                res = atomicio(vwrite, dst, &buf[start], 
                                             offset - start); 
                                if (res != (offset - start))
                                        err(1, "Writing. (pos: %ld)", start);
                                VPRINTF(2, "(%"PRId64", %"PRId64") write.\n", pos, 
                                        pos+res);
                                pos += res;
                        }

                        start = offset;

                        /* Sparse region */
                        while ( (memcmp(&buf[offset], zero_sect, 
                                       PAGE_SIZE) == 0)  && (offset < i) )
                                offset += PAGE_SIZE;
                        if (offset > i) 
                                offset = i;
                        if ((offset - start) > 0)
                        {
                                res = lseek(dst, offset - start, 
                                            SEEK_CUR);
                                if (res == (off_t)-1)
                                        err(1, "Seeking in dst (%ld)", 
                                            offset - start);
                                VPRINTF(2, "(%"PRId64", %"PRId64") skip.\n", pos, 
                                        pos+offset - start);
                                pos += (offset - start);
                        }
                }
        }
                
        if (dst_is_file && sparse)
        {
                res = ftruncate(dst, pos);
                if (res != 0)
                        err(1, "Truncating.\n");
        }

        VPRINTF(2, "Done copying\n");
        
        return;
}

int main(int argc, char *argv[])
{
        int c;
        int cs = 2048;
        int sparse = 0;
        char *src, *dst;
        int srcfd, dstfd;

        memset(zero_sect, 0, PAGE_SIZE);

        while (1) {
                int idx = 0;
                static struct option long_opts[] = {
                        {"sparse", 0, 0, 's'},
                        {"chunksize", 1, 0, 'c'},
                        {0, 0, 0, 0}
                };

                c = getopt_long (argc, argv, "+sc:v", long_opts, &idx);

                if (c == -1)
                        break;
                
                switch (c) {
                case 0:
                        printf("option %d:%s", optind,long_opts[idx].name);
                        if (optarg)
                                printf("with arg %s", optarg);
                        printf("\n");
                        break;
                case 's':
                         sparse = 1;
                         break;
                case 'c':
                        cs = atoi(optarg);
                        break;
                case 'v':
                        verbose++;
                        break;
                default:
                        printf("bad things\n");
                }
        }

        if (optind != ( argc - 2)) {
                printf("usage: %s [--sparse] [--chunksize N(KB)] "
                       "<src> <dest>\n", 
                       argv[0]);
                return -1;
        }

        src = argv[optind++];
        dst = argv[optind++];
        cs *= 1024;

        srcfd = open(src, O_RDONLY | O_DIRECT | O_LARGEFILE);
        if (srcfd == -1)
                err(1, "Opening source file (%s).", src);
        dstfd = open(dst, 
                     O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_LARGEFILE, 
                     0600);

        if (dstfd == -1)
                err(1, "Opening destination file (%s).", dst);

        dcopy(srcfd, dstfd, sparse, cs);
        
        return 0; 
}
