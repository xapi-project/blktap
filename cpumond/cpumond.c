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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "cpumond.h"

#ifndef DEBUG
#define NDEBUG
#endif
#include <assert.h>

int run;

void sighandler(int signo){
    if (signo == SIGINT)
        run = 0;
}

void cpumond_destroy(cpumond_entry_t *cpumond_entry){
    assert(cpumond_entry);

    if ((cpumond_entry->mm != NULL) && (cpumond_entry->mm != MAP_FAILED))
        if (munmap(cpumond_entry->mm, sizeof(cpumond_t)) == -1)
            perror("munmap");

    if (cpumond_entry->fd >= 0)
        if (close(cpumond_entry->fd) == -1)
            perror("close");

    if (cpumond_entry->path){
        if (shm_unlink(cpumond_entry->path) == -1)
            perror("shm_unlink");
        free(cpumond_entry->path);
    }

    free(cpumond_entry);

    return;
}

cpumond_entry_t *cpumond_create(char *path){
    cpumond_entry_t *cpumond_entry;

    assert(path);

    cpumond_entry = calloc(1, sizeof(cpumond_entry_t));
    if (!cpumond_entry){
        perror("calloc");
        goto err;
    }

    cpumond_entry->fd = shm_open(path, O_RDWR|O_CREAT|O_EXCL,
                             S_IRUSR|S_IRGRP|S_IROTH);
    if (cpumond_entry->fd == -1){
        perror("shm_open");
        goto err;
    }

    cpumond_entry->path = strdup(path);
    if (!cpumond_entry->path){
        perror("strcpy");
        goto err;
    }

    if (ftruncate(cpumond_entry->fd, sizeof(cpumond_t)) == -1){
        perror("ftruncate");
        goto err;
    }

    cpumond_entry->mm = mmap(NULL, sizeof(cpumond_t), PROT_READ | PROT_WRITE,
                         MAP_SHARED, cpumond_entry->fd, 0);
    if (cpumond_entry->mm == MAP_FAILED){
        perror("mmap");
        goto err;
    }

    return cpumond_entry;

err:
    if (cpumond_entry)
        cpumond_destroy(cpumond_entry);

    return NULL;
}

inline int statread(int statfd, long long *total, long long *idle){
    long long val[10];
    char      buf[256];
    int       i, err = 0;

    if (lseek(statfd, 0, SEEK_SET) == -1){
        err = errno;
        perror("lseek");
        goto out;
    }

    memset(buf, 0, sizeof(buf));
    if (read(statfd, buf, sizeof(buf)-1) <= 0){
        err = (errno)?errno:-1;
        perror("read");
        goto out;
    }
    if (sscanf(buf, "cpu %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
               &val[0], &val[1], &val[2], &val[3], &val[4], &val[5], &val[6],
               &val[7], &val[8], &val[9]) != 10){
        err = (errno)?errno:-1;
        perror("sscanf");
        goto out;
    }

    *idle  = val[3];
    *total = 0;
    for (i=0; i<10; i++)
        *total += val[i];

out:
    return err;
}

int cpumond_loop(cpumond_entry_t *cpumond_entry){
    long long      idle1  = 0, idle2;
    long long      total1 = 0, total2;
    int            statfd = -1;
    int            err    =  0;

    statfd = open("/proc/stat", O_RDONLY);
    if (statfd == -1){
        err = errno;
        perror("open");
        goto out;
    }

    while(run){
        if (statread(statfd, &total2, &idle2) != 0)
            goto out;

        cpumond_entry->mm->curr = 100.0 *
            ((total2-total1)-(idle2-idle1))/(total2-total1);

        cpumond_entry->mm->idle = 100 - cpumond_entry->mm->curr;

#ifdef DEBUG
        printf("total2: %lld, total1: %lld, idle2: %lld, idle1: %lld, " \
               "cpumond_entry->mm->idle: %f\n", total2, total1, idle2, idle1,
                cpumond_entry->mm->idle);
#endif

        total1 = total2;
        idle1  = idle2;

        sleep(1);
    }

    memset(cpumond_entry->mm, 0, sizeof(*(cpumond_entry->mm)));

out:
    if (statfd != -1)
        close(statfd);
    return err;
}

int main(int argc, char **argv){
    cpumond_entry_t *cpumond_entry;
    int err = EXIT_SUCCESS;

    signal(SIGINT, sighandler);

    cpumond_entry = cpumond_create(CPUMOND_PATH);
    if (!cpumond_entry){
        err = EXIT_FAILURE;
        goto out;
    }

    run = 1;
    if (cpumond_loop(cpumond_entry) != 0)
        err = EXIT_FAILURE;

out:
    if (cpumond_entry)
        cpumond_destroy(cpumond_entry);

    return err;
}
