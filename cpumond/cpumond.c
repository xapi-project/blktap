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

void cum_destroy(cum_entry_t *cum_entry){
    assert(cum_entry);

    if ((cum_entry->mm != NULL) && (cum_entry->mm != MAP_FAILED))
        if (munmap(cum_entry->mm, sizeof(cum_t)) == -1)
            perror("munmap");

    if (cum_entry->fd >= 0)
        if (close(cum_entry->fd) == -1)
            perror("close");

    if (cum_entry->path){
        if (shm_unlink(cum_entry->path) == -1)
            perror("shm_unlink");
        free(cum_entry->path);
    }

    free(cum_entry);

    return;
}

cum_entry_t *cum_create(char *path){
    cum_entry_t *cum_entry;

    assert(path);

    cum_entry = calloc(1, sizeof(cum_entry_t));
    if (!cum_entry){
        perror("calloc");
        goto err;
    }

    cum_entry->fd = shm_open(path, O_RDWR|O_CREAT|O_EXCL,
                             S_IRUSR|S_IRGRP|S_IROTH);
    if (cum_entry->fd == -1){
        perror("shm_open");
        goto err;
    }

    cum_entry->path = strdup(path);
    if (!cum_entry->path){
        perror("strcpy");
        goto err;
    }

    if (ftruncate(cum_entry->fd, sizeof(cum_t)) == -1){
        perror("ftruncate");
        goto err;
    }

    cum_entry->mm = mmap(NULL, sizeof(cum_t), PROT_READ | PROT_WRITE,
                         MAP_SHARED, cum_entry->fd, 0);
    if (cum_entry->mm == MAP_FAILED){
        perror("mmap");
        goto err;
    }

    return cum_entry;

err:
    if (cum_entry)
        cum_destroy(cum_entry);

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

int cum_loop(cum_entry_t *cum_entry){
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

        cum_entry->mm->curr = 100.0 *
            ((total2-total1)-(idle2-idle1))/(total2-total1);

        cum_entry->mm->idle = 100 - cum_entry->mm->curr;

#ifdef DEBUG
        printf("total2: %lld, total1: %lld, idle2: %lld, idle1: %lld, " \
               "cum_entry->mm->idle: %f\n", total2, total1, idle2, idle1,
                cum_entry->mm->idle);
#endif

        total1 = total2;
        idle1  = idle2;

        sleep(1);
    }

    memset(cum_entry->mm, 0, sizeof(*(cum_entry->mm)));

out:
    if (statfd != -1)
        close(statfd);
    return err;
}

int main(int argc, char **argv){
    cum_entry_t *cum_entry;
    int err = EXIT_SUCCESS;

    signal(SIGINT, sighandler);

    cum_entry = cum_create(CUM_PATH);
    if (!cum_entry){
        err = EXIT_FAILURE;
        goto out;
    }

    run = 1;
    if (cum_loop(cum_entry) != 0)
        err = EXIT_FAILURE;

out:
    if (cum_entry)
        cum_destroy(cum_entry);

    return err;
}
