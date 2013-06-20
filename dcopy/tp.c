/* tp.c
 *
 * generate a testpattern to a file.
 *
 * usage: tp <outfile> <chunksizeKB> <iterations> <pad bytes>
 *
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

int main (int argc, char *argv[])
{

        int fd;
        char *pad, *chunk0, *chunk1;
        int i, cs, ps;

        fd = open(argv[1], 
                  O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT | O_LARGEFILE,
                  0600);
        
        ps =  atoi(argv[4]);
        posix_memalign((void **)&pad, 4096, ps);
        memset(pad, 1, ps);

        cs = atoi(argv[2]) * 1024;
        posix_memalign((void **)&chunk0, 4096, cs);
        posix_memalign((void **)&chunk1, 4096, cs);
        i = atoi(argv[3]);

        memset(chunk0, 0, cs);
        memset(chunk1, 0xff, cs);

        write(fd, pad, ps);
        while (i > 0) {
                write(fd, chunk0, cs);
                write(fd, chunk1, cs);
                i--;
        }

        return 0;
}
