/*
 *  Copyright (c) 2007 XenSource Inc.
 *  All rights reserved.
 *
 */

/*
 * This module implements a "dot locking" style advisory file locking algorithm.
 */

/* TODO: 
  o deal w/ errors better
  o return error codes(?) through errno(?)
 */

#if defined(EXCLUSIVE_LOCK)
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "lock.h"

#define unlikely(x) __builtin_expect(!!(x), 0)

/* format: xenlk.hostname.uuid.timebit */
#define LF_POSTFIX ".xenlk"
#define LFL_FORMAT LF_POSTFIX ".%s.%s.%x"
#define RETRY_MAX 16

#if defined(LOGS)
#define LOG(format, args...) printf(format, ## args)
#else
#define LOG(format, args...)
#endif

#define XSLEEP usleep(random() & 0x7ffff)

static char *create_lockfn(char *fn_to_lock)
{
        char *lockfn;
    
        /* allocate string to hold constructed lock file */
        lockfn = malloc(strlen(fn_to_lock) + strlen(LF_POSTFIX) + 1);
        if (unlikely(!lockfn)) {
                errno = ENOMEM; 
                return 0;
        }

        /* append postfix to file to lock */
        strcpy(lockfn, fn_to_lock);
        strcat(lockfn, LF_POSTFIX);

        return lockfn;
}

static char *create_lockfn_link(char *fn_to_lock, char *uuid)
{
        char hostname[128];
        char *lockfn_link;
        char *ptr;

        /* get hostname */
        if (unlikely(gethostname(hostname, sizeof(hostname)) == -1)) {
                return 0;
        }

        /* allocate string to hold constructed lock file link */
        lockfn_link = malloc(strlen(fn_to_lock) + strlen(LF_POSTFIX) +
                             strlen(hostname) + strlen(uuid) + 8);
        if (unlikely(!lockfn_link)) {
                errno = ENOMEM; 
                return 0;
        }

        /* construct lock file link with specific format */
        strcpy(lockfn_link, fn_to_lock);
        ptr = lockfn_link + strlen(lockfn_link);
        sprintf(ptr, LFL_FORMAT, hostname, uuid, (int)time(0) & 0xf);

        return lockfn_link;
}

int lock(char *fn_to_lock, char *uuid, int force)
{
        char *lockfn = 0;
        char *lockfn_link = 0;
        char *buf = 0;
        int fd = -1;
        int status = -1;
        struct stat stat1, stat2;
        int retry_attempts = 0;
        int clstat;
    
        if (!fn_to_lock || !uuid)
                return status;

        /* build lock file strings */
        lockfn = create_lockfn(fn_to_lock);
        if (unlikely(!lockfn)) goto finish;

try_again:
        if (retry_attempts++ > RETRY_MAX) goto finish;
        free(lockfn_link);
        lockfn_link = create_lockfn_link(fn_to_lock, uuid);
        if (unlikely(!lockfn_link)) goto finish;

        /* try to open lockfile */
        fd = open(lockfn, O_WRONLY | O_CREAT | O_EXCL, 0644); 
        if (fd == -1) {
                LOG("Initial lockfile creation failed %s force=%d\n", 
                     lockfn, force);
                /* force gets first chance */
                if (force) {
                        /* assume the caller knows when forcing is necessary */
                        status = unlink(lockfn);
                        if (unlikely(status == -1)) {
                                LOG("force removal of %s lockfile failed, "
                                    "errno=%d, trying again\n", lockfn, errno);
                        }
                        goto try_again;
                }
                /* already owned? (hostname & uuid match, skip time bits) */
                fd = open(lockfn, O_RDWR, 0644);
                if (fd != -1) {
                        buf = malloc(strlen(lockfn_link)+1);
                        if (read(fd, buf, strlen(lockfn_link)) != 
                           (strlen(lockfn_link))) {
                                clstat = close(fd);
                                if (unlikely(clstat == -1)) {
                                        LOG("fail on close\n");
                                }
                                free(buf);
                                goto try_again;
                        }
                        if (!strncmp(buf, lockfn_link, strlen(lockfn_link)-1)) {
                                LOG("lock owned by us, reasserting\n");
                                /* our lock, reassert by rewriting below */
                                if (lseek(fd, 0, SEEK_SET) == -1) {
                                        clstat = close(fd);
                                        if (unlikely(clstat == -1)) {
                                                LOG("fail on close\n");
                                        }
                                        goto finish;
                                }
                                free(buf);
                                goto skip;
                        }
                        free(buf);
                        clstat = close(fd);
                        if (unlikely(clstat == -1)) {
                                LOG("fail on close\n");
                        }
                }
                goto finish;
        }

        LOG("lockfile created %s\n", lockfn);

skip:
        /* 
         * write the name of the temporary lock -- it contains 
         * uuid and hostname, useful for reasserting lock.
         */
        if (write(fd, lockfn_link, strlen(lockfn_link)) != 
                strlen(lockfn_link)) {
                clstat = close(fd);
                if (unlikely(clstat == -1)) {
                        LOG("fail on close\n");
                }
                retry_attempts = 0;
                goto try_again;
        }
        clstat = close(fd);
        if (unlikely(clstat == -1)) {
                LOG("fail on close\n");
        }

        while (retry_attempts++ < RETRY_MAX) {
                int st = link(lockfn, lockfn_link);
                LOG("linking %s and %s\n", lockfn, lockfn_link);
                if (unlikely(st == -1)) { 
                        LOG("link status is %d, errno=%d\n", st, errno); 
                }

                if (lstat(lockfn, &stat1) == -1) {
                        status = -1;
                        goto finish;
                }

                if (lstat(lockfn_link, &stat2) == -1) {
                        status = -1;
                        goto finish;
                }

                /* compare inodes */
                if (stat1.st_ino == stat2.st_ino) {
                        /* success, inodes are the same */
                        /* should we check that st_nlink's are also 2?? */
                        status = 0;
                        unlink(lockfn_link);
                        goto finish;
                } else {
                        /* try again */
                        unlink(lockfn);
                        unlink(lockfn_link);
                        retry_attempts = 0;
                        goto try_again;
                }
        }

finish:
        free(lockfn);
        free(lockfn_link);

        LOG("returning status %d, errno=%d\n", status, errno);
        return status;
}


int unlock(char *fn_to_unlock, char *uuid)
{
        char *lockfn = 0;
        char *lockfn_link = 0;
        char *buf = 0;
        int buflen;
        int readlen;
        int status = -1;
        int fd;
        int clstat;

        if (!fn_to_unlock || !uuid)
                return status;

        /* build lock file string */
        lockfn = create_lockfn(fn_to_unlock);
        if (unlikely(!lockfn)) goto finish;

        /* build comparitor */
        lockfn_link = create_lockfn_link(fn_to_unlock, uuid);
        if (unlikely(!lockfn_link)) goto finish;

        /* open and read lock owner */
        fd = open(lockfn, O_RDONLY);
        if (fd == -1) {
                if (errno == ENOENT)
                        status = 0;
                goto finish;
        }

        buflen = strlen(lockfn_link);
        buf = malloc(buflen);
        readlen = read(fd, buf, buflen);
        if (unlikely(readlen < 2)) {
                clstat = close(fd);
                if (unlikely(clstat == -1)) {
                        LOG("fail on close\n");
                }
                goto finish;
        }

        clstat = close(fd);
        if (unlikely(clstat == -1)) {
                LOG("fail on close\n");
        }

        /* if we own, remove file */
        if (!strncmp(buf, lockfn_link, 
                (readlen < buflen) ? readlen - 1: buflen - 1)) {
                if (unlink(lockfn) == -1)
                        if (errno != ENOENT) {
                                goto finish;
                        }
        }

        status = 0;

finish:
        free(lockfn);
        free(lockfn_link);
        free(buf);
        return status;
}

int lock_delta(char *fn_to_check)
{
        char *lockfn = 0;
        struct stat statbuf, statnow;
        int result = -1;
        int uniq, fd;
        char *buf = 0;
        int clstat;
        int pid = (int)getpid();

        if (!fn_to_check)
                return result;

        /* build lock file string */
        lockfn = create_lockfn(fn_to_check);
        if (unlikely(!lockfn)) goto finish;
        if (lstat(lockfn, &statbuf) == -1) goto finish;

        srandom((int)time(0) ^ pid);
        uniq = random() % 0xffffff; 
        buf = malloc(strlen(fn_to_check) + 24);
        if (unlikely(!buf)) goto finish;

        strcpy(buf, fn_to_check);
        sprintf(buf + strlen(buf), ".xen%08d.tmp", uniq);

        fd = open(buf, O_WRONLY | O_CREAT, 0644);
        if (fd == -1) goto finish;
        clstat = close(fd);
        if (clstat == -1) {
                LOG("fail on close\n");
        }
        if (lstat(buf, &statnow) == -1) {
                unlink(buf);
                goto finish;
        }
        unlink(buf);
         
        result = (int)statnow.st_mtime - (int)statbuf.st_mtime;
        if (result < 0) result = 0;

finish:
        free(buf);
        return result;
}



/*
 * the following is for sanity testing. 
 */

#if defined(TEST)
static void usage(char *prg)
{
        printf("usage %s [dwtru <filename>] [l <filename> (0|1)]\n", prg);
}

static void test_file(char *fn)
{
        FILE *fptr;
        int prev_count = 0;
        int count, pid, time;

        fptr = fopen(fn, "r");
        if (!fptr) {
                LOG("ERROR on file %s open, errno=%d\n", fn, errno);
                return;
        } 

        while (!feof(fptr)) {
                fscanf(fptr, "%d %d %d\n", &count, &pid, &time);
                if (prev_count != count) {
                        LOG("ERROR: prev_count=%d, count=%d, pid=%d, time=%d\n",
                                    prev_count, count, pid, time);
                }
                prev_count = count + 1;
        }
}

static void random_locks(char *fn)
{
        int pid = getpid();
        int status;
        char *filebuf = malloc(256);
        int count = 0;
        int dummy;
        int clstat;
        char uuid[12];

        /* this will never return, kill to exit */

        srandom((int)time(0) ^ pid);

        LOG("pid: %d using file %s\n", pid, fn);
        sprintf(uuid, "%08d", pid);

        while (1) {
                XSLEEP;
                status = lock(fn, uuid, 0);
                if (status == 0) {
                        /* got lock, open, read, modify write close file */
                        int fd = open(fn, O_RDWR, 0644);
                        if (fd == -1) {
                                LOG("pid: %d ERROR on file %s open, errno=%d\n", 
                                    pid, fn, errno);
                        } else {
                                /* ugly code to read data in test format */
                                /* format is "%d %d %d" 'count pid time' */
                                struct stat statbuf;
                                int bytes;
                                status = stat(fn, &statbuf);
                                if (status != -1) {
                                        if (statbuf.st_size > 256) {
                                                lseek(fd, -256, SEEK_END);
                                        } 
                                        memset(filebuf, 0, 256);
                                        bytes = read(fd, filebuf, 256);
                                        if (bytes) {
                                                int bw = bytes-2;
                                                while (bw && filebuf[bw]!='\n') 
                                                        bw--;
                                                if (!bw) bw = -1;
                                                sscanf(&filebuf[bw+1], 
                                                       "%d %d %d", 
                                                       &count, &dummy, &dummy);
                                                count += 1;
                                        }
                                        lseek(fd, 0, SEEK_END);
                                        sprintf(filebuf, "%d %d %d\n", 
                                                count, pid, (int)time(0));
                                        write(fd, filebuf, strlen(filebuf));
                                } else {
                                        LOG("pid: %d ERROR on file %s stat, "
                                            "errno=%d\n", pid, fn, errno);
                                }

                                clstat = close(fd);
                                if (clstat == -1) {
                                        LOG("fail on close\n");
                                }
                        }
                        XSLEEP;
                        status = unlock(fn, uuid);
                        LOG("unlock status is %d\n", status);
                }
        }
}

static void perf_lock(char *fn, int loops)
{
    int status;
    char buf[9];
    int start = loops;

    sprintf(buf, "%08d", getpid());

    while (loops--) {
        status = lock(fn, buf, 0);
        if (status == -1) {
            printf("failed to get lock at iteration %d\n", start - loops);
            return;
        }
    }
    unlock(fn, buf);
}

int main(int argc, char *argv[])
{
        int status;
        char uuid[12];

        if (argc < 3) {
                usage(argv[0]);
                return 0;
        }

        sprintf(uuid, "%08d", getpid());

        if (!strcmp(argv[1],"d")) {
                status = lock_delta(argv[2]);
                printf("lock delta for %s failed, errno=%d\n", argv[2], status);
        } else if (!strcmp(argv[1],"t")) {
                test_file(argv[2]);
        } else if (!strcmp(argv[1],"r")) {
                random_locks(argv[2]);
        } else if (!strcmp(argv[1],"p")) {
                perf_lock(argv[2], argc < 3 ? 100000 : atoi(argv[3]));
        } else if (!strcmp(argv[1],"l")) {
                status = lock(argv[2], uuid, argc < 4 ? 0 : atoi(argv[3]));
                printf("lock status = %d\n", status);
        } else if (!strcmp(argv[1],"u") && (argc == 3)) {
                status = unlock(argv[2], uuid);
                printf("unlock status = %d\n", status);
        } else {
                usage(argv[0]);
        }

        return 0;
}
#endif
#endif
