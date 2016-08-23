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

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "tapdisk-metrics.h"
#include "tapdisk-log.h"
#include "debug.h"
#include "tapdisk-queue.h"
#include "td-req.h"

#define VBD_STATS_VERSION 0x00000001

/* make a static metrics struct, so it only exists in the context of this file */
static td_metrics_t td_metrics;

/* Returns 0 in case there were no problems while emptying the folder */
static int
empty_folder(char *path)
{
    DIR *dir;
    struct dirent *direntry;
    struct stat statbuf;
    char *file = NULL;
    int err = 0;

    dir = opendir(path);
    if (!dir){
        err = errno;
        EPRINTF("failed to open directory: %s\n", strerror(err));
        goto out;
    }

    while((direntry = readdir(dir)) != NULL){
        if(!strcmp(direntry->d_name, ".") || !strcmp(direntry->d_name, ".."))
            continue;

        err = asprintf(&file, "%s/%s", path, direntry->d_name);
        if (unlikely(err == -1)) {
            free(file);
            file = NULL;	
            err = errno;
            EPRINTF("failed to allocate file path name in memory to delete: %s\n",
                strerror(err));
            goto out;
        }
        stat(file, &statbuf);
        if(statbuf.st_mode & S_IFREG){
            unlink(file);
        }else{
            empty_folder(file);
            rmdir(file);
        }
        free(file);
        file = NULL;
    }

out:
    if (dir)
        closedir(dir);

    return err;
}

int
td_metrics_start()
{
    int err = 0;

    err = asprintf(&td_metrics.path, TAPDISK_METRICS_PATHF, getpid());
    if (unlikely(err == -1)) {
        err = errno;
        EPRINTF("failed to allocate metric's folder path name in memory: %s\n",
            strerror(err));
        td_metrics.path = NULL;
        goto out;
    }

    err = mkdir(td_metrics.path, S_IRWXU);
    if (unlikely(err == -1)) {
        if (errno == EEXIST) {
            /* In case there is a previous folder with the same pid,
             * we empty it and use it for the new tapdisk instance.
             */
            err = 0;
            empty_folder(td_metrics.path);
        }else{
            EPRINTF("failed to create folder to store metrics: %s\n", strerror(err));
        }
    }

out:
    return err;
}

void
td_metrics_stop()
{
    if (!td_metrics.path)
        goto out;

    empty_folder(td_metrics.path);

    if (rmdir(td_metrics.path) == -1){
        EPRINTF("failed to delete metrics folder: %s\n", strerror(errno));
        goto out;
    }

    free(td_metrics.path);
    td_metrics.path = NULL;

out:
    return;
}

int
td_metrics_vdi_start(int minor, stats_t *vdi_stats)
{
    int err = 0;

    if(!td_metrics.path)
        goto out;

    shm_init(&vdi_stats->shm);

    err = asprintf(&vdi_stats->shm.path, TAPDISK_METRICS_VDI_PATHF,
            td_metrics.path, minor);

    if(unlikely(err == -1)){
        err = errno;
        EPRINTF("failed to allocate memory to store vdi metrics path: %s\n",
            strerror(err));
        vdi_stats->shm.path = NULL;
        goto out;
    }

    vdi_stats->shm.size = PAGE_SIZE;

    err = shm_create(&vdi_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to create shm ring stats file: %s\n", strerror(err));
        goto out;
   }

    vdi_stats->stats = vdi_stats->shm.mem;

out:
    return err;
}

int
td_metrics_vdi_stop(stats_t *vdi_stats)
{
    int err = 0;

    if(!vdi_stats->shm.path)
        goto end;

    err = shm_destroy(&vdi_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to destroy vdi metrics file: %s\n", strerror(err));
    }

    free(vdi_stats->shm.path);
    vdi_stats->shm.path = NULL;

end:
    return err;
}
int
td_metrics_vbd_start(int domain, int id, stats_t *vbd_stats)
{
    int err = 0;

    if(!td_metrics.path)
        goto out;

    shm_init(&vbd_stats->shm);

    err = asprintf(&vbd_stats->shm.path, TAPDISK_METRICS_VBD_PATHF,
            td_metrics.path, domain, id);
    if(unlikely(err == -1)){
        err = errno;
        EPRINTF("failed to allocate memory to store vbd metrics path: %s\n",
            strerror(err));
        vbd_stats->shm.path = NULL;
        goto out;
    }

    vbd_stats->shm.size = PAGE_SIZE;

    err = shm_create(&vbd_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to create shm ring stats file: %s\n", strerror(err));
        goto out;
   }
    vbd_stats->stats = vbd_stats->shm.mem;
    vbd_stats->stats->version = VBD_STATS_VERSION;
out:
    return err;

}

int
td_metrics_vbd_stop(stats_t *vbd_stats)
{
    int err = 0;

    if(!vbd_stats->shm.path)
        goto end;

    err = shm_destroy(&vbd_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to destroy vbd metrics file: %s\n", strerror(err));
    }

    free(vbd_stats->shm.path);
    vbd_stats->shm.path = NULL;

end:
    return err;
}

int
td_metrics_blktap_start(int minor, stats_t *blktap_stats)
{

    int err = 0;

    if(!td_metrics.path)
        goto out;

    shm_init(&blktap_stats->shm);

    err = asprintf(&blktap_stats->shm.path, TAPDISK_METRICS_BLKTAP_PATHF, td_metrics.path, minor);
    if(unlikely(err == -1)){
        err = errno;
        EPRINTF("failed to allocate memory to store blktap metrics path: %s\n",strerror(err));
        blktap_stats->shm.path = NULL;
        goto out;
    }

    blktap_stats->shm.size = PAGE_SIZE;

    err = shm_create(&blktap_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to create blktap shm ring stats file: %s\n", strerror(err));
        goto out;
    }
    blktap_stats->stats = blktap_stats->shm.mem;
out:
    return err;
}

int
td_metrics_blktap_stop(stats_t *blktap_stats)
{
    int err = 0;

    if(!blktap_stats->shm.path)
        goto end;

    err = shm_destroy(&blktap_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to destroy blktap metrics file: %s\n", strerror(err));
    }

    free(blktap_stats->shm.path);
    blktap_stats->shm.path = NULL;

end:
    return err;

}

int
td_metrics_nbd_start(stats_t *nbd_stats, int minor)
{
    int err = 0;

    if(!td_metrics.path || nbd_stats->shm.path != NULL)
        goto out;

    shm_init(&nbd_stats->shm);

    err = asprintf(&nbd_stats->shm.path, TAPDISK_METRICS_NBD_PATHF, td_metrics.path, minor);
    if(unlikely(err == -1)){
        err = errno;
        EPRINTF("failed to allocate memory to store NBD metrics path: %s\n",strerror(err));
        nbd_stats->shm.path = NULL;
        goto out;
    }

    nbd_stats->shm.size = PAGE_SIZE;

    err = shm_create(&nbd_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to create NBD shm ring stats file: %s\n", strerror(err));
        goto out;
   }
    nbd_stats->stats = nbd_stats->shm.mem;
out:
    return err;
}

int
td_metrics_nbd_stop(stats_t *nbd_stats)
{
    int err = 0;

    if(!nbd_stats->shm.path)
        goto end;
    err = shm_destroy(&nbd_stats->shm);
    if (unlikely(err)) {
        err = errno;
        EPRINTF("failed to destroy NBD metrics file: %s\n", strerror(err));
    }

    free(nbd_stats->shm.path);
    nbd_stats->shm.path = NULL;

end:
    return err;
}
