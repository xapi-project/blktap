/*
 * XenRT: Small disk test utility. We write the sector number to
 * to each successive sector. The verify flag checks for incorrect
 * sector entries.
 *
 * Julian Chesterfield, July 2007
 *
 * Copyright (c) 2007 XenSource, Inc. All use and distribution of this
 * copyrighted material is governed by and subject to terms and
 * conditions as licensed by XenSource, Inc. All other rights reserved.
 *
 */


#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <string.h>
#include "atomicio.h"
#include <time.h>

#define DEFAULT_SECTOR_SIZE 512
#define SECTOR_SHIFT 9

unsigned long long iter = 0;

struct fd_state {
        unsigned long      sector_size; // size of a sector
        unsigned long long size; // device size in sectors
	unsigned long long size_sects; // device size in sectors
	unsigned long long fullsize; // full size of the device
};

struct sector_hdr {
	unsigned long long sect;
	unsigned long long iter;
};

int usage(char *str) {
	fprintf(stderr, "usage: %s {write|verify|report} <iterations> <FILENAME>\n",
		str);
	exit(1);
}

void fill_buf(char *buf, struct sector_hdr *hdr, int size) {
	int i;
	for(i=0; i<(size/sizeof(struct sector_hdr)); i++)
		memcpy(buf + (sizeof(struct sector_hdr) * i), hdr, sizeof(struct sector_hdr));
}

static int getsize(int fd, struct fd_state *s) {
        struct stat stat;
	int ret;

        ret = fstat(fd, &stat);
        if (ret != 0) {
                fprintf(stderr, "\nERROR: fstat failed, Couldn't stat image");
                return -EINVAL;
        }

	if (S_ISBLK(stat.st_mode)) {
                /*Accessing block device directly*/
                s->size = 0;
                if (ioctl(fd,BLKGETSIZE,&s->size)!=0) {
                        fprintf(stderr,"\nERR: BLKGETSIZE failed, "
                                 "couldn't stat image");
                        return -EINVAL;
                }
		s->size_sects = s->size;
                /*Get the sector size*/
#if defined(BLKSSZGET)
                {
                        s->sector_size = DEFAULT_SECTOR_SIZE;
                        ioctl(fd, BLKSSZGET, &s->sector_size);
                }
#else
                s->sector_size = DEFAULT_SECTOR_SIZE;
#endif
		if (s->sector_size != DEFAULT_SECTOR_SIZE) {
			if (s->sector_size > DEFAULT_SECTOR_SIZE) {
				s->size_sects = 
					(s->sector_size/DEFAULT_SECTOR_SIZE)*s->size;
			} else {
				s->size_sects = 
					s->size/(DEFAULT_SECTOR_SIZE/s->sector_size);
			}
		}
		s->fullsize = s->sector_size * s->size;

        } else {
                /*Local file? try fstat instead*/
                s->size = (stat.st_size >> SECTOR_SHIFT);
                s->sector_size = DEFAULT_SECTOR_SIZE;
		s->size_sects = s->size;
		s->fullsize = stat.st_size;
        }
        return 0;
}

//int verify_testpattern(int fd, struct fd_state *state, int bReportOnly = 0, int *writeEstimate = NULL);

int verify_testpattern(int fd, struct fd_state *state, int bReportOnly, int *writeEstimate) 
{
	int running = 1, len, ret, j;
	unsigned long long sects, i;
	struct sector_hdr *test;
	char *buf;
	clock_t start = clock(), finish;

	buf = malloc(DEFAULT_SECTOR_SIZE);
	if (!buf) 
	{
		fprintf(stderr, "\nMalloc failed\n");
		return -1;
	}
	sects = state->size_sects;
	ret = i = 0;
    while (running) 
	{

		if (!(i % 1048576)) 
		{
			printf("\nVerifying sector %llu of %llu\n", i, sects);
		}

		if (bReportOnly)
			start = clock();
		
		/*Attempt to read the data*/
		if (lseek(fd, i * DEFAULT_SECTOR_SIZE, SEEK_SET)==(off_t)-1) 
		{
			fprintf(stderr,"\nUnable to seek to offset %llu (%d)\n",
				i * DEFAULT_SECTOR_SIZE, errno);
			return -1;
		}
		len = atomicio(read, fd, buf, DEFAULT_SECTOR_SIZE);
		if (len < DEFAULT_SECTOR_SIZE) 
		{
			fprintf(stderr, "\nRead failed %llu\n",
				 (long long unsigned) i);
			return -1;
		}
		for(j=0; j<DEFAULT_SECTOR_SIZE/sizeof(struct sector_hdr); j++) 
		{
			test = (struct sector_hdr *)(buf + (sizeof(struct sector_hdr) * j));
			if (test->sect != i) 
			{
				printf("Val is %llu\n",test->sect);
				fprintf(stderr, "\nSector %llu, off %d:\n"
					"Sector number does not match\n",
					(long long unsigned) i, (sizeof(struct sector_hdr) * j));
				return 1;
			}
			if (test->iter != iter) 
			{
				fprintf(stderr, "\nSector %llu, off %d:\n"
					"Iteration number does not match\n",
					(long long unsigned) i, (sizeof(struct sector_hdr) * j));
				return 2;				
			}
		}

		if (bReportOnly)
			finish = clock();
			*writeEstimate = sects * ((finish - start)/CLOCKS_PER_SEC);
			break;

	i++;
	if (i >= sects)
		running = 0;
	}
	free(buf);
	return ret;
}

//int write_testpattern(int fd, struct fd_state *state, int bReportOnly = 0, int *writeEstimate = NULL);

int write_testpattern(int fd, struct fd_state *state, int bReportOnly, int *writeEstimate) 
{
	int running = 1, len;
	unsigned long long sects, i;
	struct sector_hdr hdr;
	char *buf;
	clock_t start = clock(), finish;		

	buf = malloc(DEFAULT_SECTOR_SIZE);
	if (!buf) {
		fprintf(stderr, "\nMalloc failed\n");
		return -1;
	}

	sects = state->size_sects;
	i = 0;
    while (running) 
	{

		if (!(i % 1048576)) 
		{
			printf("Writing sector %llu of %llu\n", i, sects);
		}

		if (bReportOnly)
			start = clock();		
	
		/*Attempt to write the data*/
		if (lseek(fd, i * DEFAULT_SECTOR_SIZE, SEEK_SET)==(off_t)-1) 
		{
			fprintf(stderr,"\nUnable to seek to offset %llu\n",
				i * DEFAULT_SECTOR_SIZE);
			return -1;
		}	
		hdr.sect = i;
		hdr.iter = iter;
		fill_buf(buf, &hdr, DEFAULT_SECTOR_SIZE);
		len = atomicio(vwrite, fd, buf, DEFAULT_SECTOR_SIZE);
		if (len < DEFAULT_SECTOR_SIZE) 
		{
			fprintf(stderr, "\nWrite failed %llu\n",
				 (long long unsigned) i);
			return -1;
		}

		if (bReportOnly)
			finish = clock();
			*writeEstimate = sects * ((finish - start)/CLOCKS_PER_SEC);
			break;

		i++;
		if (i >= sects)
			running = 0;
	}
	free(buf);
	return 0;
}

int main(int argc, char *argv[])
{
	int fd = 0, retval = 0;
	int o_flags = O_LARGEFILE;
	struct fd_state *state = calloc(sizeof(struct fd_state),1);
	int writeEstimate = 0;
	int verifyEstimate = 0;

	printf("into the file at least.");
	if (argc != 4)
		usage(argv[0]);

	iter = strtoull(argv[2],NULL,10);

	if (!strcmp(argv[1],"write")) {
		fd = open(argv[3], O_RDWR | o_flags);
		if (fd == -1) {
			fprintf(stderr,"\nUnable to open [%s], (err %d)!\n",argv[3],0 - errno);
			return 1;
		}
		if (getsize(fd, state)!=0)
			return -1;
		retval = write_testpattern(fd, state, 0, NULL);
	} else if (!strcmp(argv[1],"verify")) {
		fd = open(argv[3], O_RDONLY | o_flags);
		if (fd == -1) {
			fprintf(stderr,"\nUnable to open [%s], (err %d)!\n",argv[3],0 - errno);
			return 1;
		}
		if (getsize(fd, state)!=0)
			return -1;
		retval = verify_testpattern(fd, state, 0, NULL);
	} else if (!strcmp(argv[1],"report")) 
		{			
			fd = open(argv[3], O_RDWR | o_flags);
			if (fd == -1) 
			{
				fprintf(stderr,"\nUnable to open [%s], (err %d)!\n",argv[3],0 - errno);
				return 1;
			}
			if (getsize(fd, state)!=0)
				return -1;
			retval = write_testpattern(fd, state, 1, &writeEstimate);
			retval = verify_testpattern(fd, state, 1, &verifyEstimate);
			printf("Estimated time: %s", (writeEstimate + verifyEstimate));
		}
	else 
		usage(argv[0]);


	close(fd);
	return retval;
}
