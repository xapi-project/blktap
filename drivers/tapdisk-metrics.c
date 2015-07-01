/*
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
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
#include "lock.h"
#include "tapdisk-log.h"

/* make a static metrics struct, so it only exists in the context of this file */
static td_metrics_t td_metrics;

/* Returns 0 in case there were no problems while emptying the folder */
static int
empty_folder(char *path)
{
    DIR *dir;
    struct dirent *direntry;
    struct stat statbuf;
    char *file;
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
            err = errno;
            EPRINTF("failed to allocate file path name in memory to delete: %s\n", strerror(err));
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
        EPRINTF("failed to allocate metric's folder path name in memory: %s\n", strerror(err));
        td_metrics.path = NULL;
        goto out;
    }

    err = mkdir(td_metrics.path, S_IRWXU);
    if (unlikely(err == -1)) {
        if (errno == EEXIST) {
            //In case there is a previous folder with the same pid, we empty it and use it for the new tapdisk instance.
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
