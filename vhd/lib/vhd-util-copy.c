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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "libvhd.h"
#include "vhd-util.h"

/**
 * Switches on the corresponding bit if a block has been written to in any
 * file in the chain.
 *
 * @param ctx the VHD context of the leaf
 * @param bitmap sufficient memory to store the bitmap (1 bit per block), must
 * be initialised to zero
 * @return 0 on success
 */
static int
vhd_get_used_blocks(vhd_context_t *ctx, unsigned long long *bitmap) {

    vhd_context_t *cur = NULL;
    uint64_t i;
    int err = 0;

    assert(ctx);
    assert(bitmap);

    err = vhd_get_bat(ctx);
    if (err) {
        fprintf(stderr, "failed to get BAT for %s: %s\n", ctx->file,
                strerror(-err));
        goto out;
    }

    list_for_each_entry(cur, &ctx->next, next) {
        err = vhd_get_bat(cur);
        if (err) {
            fprintf(stderr, "failed to get BAT for %s: %s\n", 
                    cur->file, strerror(-err));
            goto out;
        }
    }

    for (i = 0; i < ctx->bat.entries; i++) {
        uint64_t blk = ctx->bat.bat[i];
        if (blk == DD_BLK_UNUSED) {
            list_for_each_entry(cur, &ctx->next, next) {
                if (cur->bat.bat[i] != DD_BLK_UNUSED) {
                    blk = cur->bat.bat[i];
                    break;
                }
            }
        }
        if (blk != DD_BLK_UNUSED) {
            bitmap[(i >> 6)] |= 1u << (i % 64);
        }
    }
out:
    return 0;
}

/**
 * Returns the deepest node in the chain 
 * that has any valid data. 
 *
 * @param ctx the leaf VHD file
 * @param bat map containing the deepest VHD node 
 *        with valid data for each block
 */
static int
vhd_get_used_blocks2(vhd_context_t *ctx, vhd_context_t **bat) {

    int err = 0;
    uint32_t i = 0;
    vhd_context_t *cur = NULL;

    assert(ctx);
    assert(bat);

    err = vhd_get_bat(ctx);
    if (err) 
        goto out;

    list_for_each_entry(cur, &ctx->next, next) {
        err = vhd_get_bat(cur);
        if (err) 
            goto out;
    }

    for (i = 0; i < ctx->bat.entries; i++) {
        if (ctx->bat.bat[i] == DD_BLK_UNUSED) {
            bat[i] = NULL;
            list_for_each_entry(cur, &ctx->next, next) {
                if (cur->bat.bat[i] != DD_BLK_UNUSED) {
                    bat[i] = cur;
                    break;
                }
            }
        } else {
            bat[i] = ctx;
        }
    }
out:
    return err;
}

static inline int
_vhd_util_copy(const char *name, const int fd) {

    int err = 0, i = 0;
    vhd_context_t ctx;
    unsigned long long *bitmap = NULL;
    void *buf = NULL;
    int bs = 0; /* block size */

    assert(name);

    err = vhd_open(&ctx, name, VHD_OPEN_RDONLY | VHD_OPEN_CACHED);
    if (err) {
        fprintf(stderr, "failed to open %s: %s\n", 
                name, strerror(-err));
        return err;
    }

    err = vhd_get_bat(&ctx);
    if (err) {
        fprintf(stderr, "failed to get BAT for %s: %s\n", ctx.file,
                strerror(-err));
        goto out;
    }

    bitmap = calloc(sizeof(unsigned long long), ctx.bat.entries >> 6);
    if (!bitmap) {
        fprintf(stderr, "failed to allocate memory for the bitmap\n");
        err = -ENOMEM;
        goto out;
    }

    err = vhd_get_used_blocks(&ctx, bitmap);
    if (err) {
        fprintf(stderr, "failed to get bitmap: %s\n", strerror(-err));
        goto out;
    }

    bs = ctx.spb << 9;
    buf = malloc(bs);
    if (!buf) {
        fprintf(stderr, "failed to allocate input buffer\n");
        err = -ENOMEM;
        goto out;
    }

    for (i = 0; i < ctx.bat.entries; i++) {
        const int mask = 1u << (i % 64);
        const off64_t offset = (off64_t)i * (off64_t)bs;
        if ((bitmap[(i >> 6)] & mask) == mask) {
            const off64_t _offset = lseek64(fd, offset, SEEK_SET);
            if ((off64_t) - 1 == _offset) {
                err = -errno;
                fprintf(stderr, "failed to seek at %llu: %s\n",
                        (unsigned long long)offset,
                        strerror(-err));
                goto out;
            }
            err = read(fd, buf, bs);
            if (err == -1) {
                err = -errno;
                fprintf(stderr, 
                        "failed to read at offset %llu: %s\n",
                        (unsigned long long)offset, 
                        strerror(-err));
                goto out;
            }
        }
    }
out:
    free(bitmap);
    vhd_close(&ctx);
    free(buf);

    return err;
}

static int
vhd_footer_offset_at_eof(vhd_context_t *ctx, off64_t *off)
{
    int err;
    if ((err = vhd_seek(ctx, 0, SEEK_END)))
        return errno;
    *off = vhd_position(ctx) - sizeof(vhd_footer_t);
    return 0;
}

/**
 * Checks bitmap for given block and reads
 * data from the right file in the chain
 * starting the VHD file passed in as param
 *
 * @param ctx source VHD file
 * @param block block to be read
 * @param buffer buffer to read data into
 */
static inline int
my_vhd_read_block(vhd_context_t *ctx, uint32_t block, char *buf)
{
    int err = 0;
    size_t size;
    uint64_t blk;
    off64_t end = 0, off;
    int bit = 0, i, level;
    vhd_context_t *cur = NULL;
    char *bitmaps[30], *buf2;

    /* Initiatialze bitmap pointers */
    for(i=0; i<30; i++)
        bitmaps[i] = NULL;

    if (!vhd_type_dynamic(ctx))
        return -EINVAL;

    err = vhd_get_bat(ctx);
    if (err)
        return err;

    if (block >= ctx->bat.entries)
        return -ERANGE;

    blk  = ctx->bat.bat[block];
    if (blk == DD_BLK_UNUSED)
        return -EINVAL;

    off  = vhd_sectors_to_bytes(blk + ctx->bm_secs);
    size = vhd_sectors_to_bytes(ctx->spb);

    err  = vhd_footer_offset_at_eof(ctx, &end);
    if (err)
        return err;

    if (end < off + ctx->header.block_size) {
        size = end - off;
        memset(buf + size, 0, ctx->header.block_size - size);
    }

    /* Check if full block is available at this level */
    err = vhd_get_batmap(ctx);
    if (err) {
        fprintf(stderr, "failed to get batmap\n");
        return err;
    }
    bit = vhd_has_batmap(ctx) && vhd_batmap_test(ctx, &ctx->batmap, block);

    if (bit) {
        /* 
         * Full block is available at this level.
         * Read and return without going further
         * up in the VHD chain.
         */
        err  = vhd_seek(ctx, off, SEEK_SET);
        if (err) 
            goto out;

        err = vhd_read(ctx, buf, size);
        if (err)
            goto out;

        return 0;
    }

    err  = posix_memalign((void *)&buf2, VHD_SECTOR_SIZE,
            vhd_sectors_to_bytes(ctx->spb));
    if (err) {
        err = -err;
        goto out;
    }

    /* Read one sector at a time */
    size = vhd_sectors_to_bytes(1);

    if (bitmaps[0]==NULL) {
        err = vhd_read_bitmap(ctx, block, &bitmaps[0]);
        if (err) 
            goto out;

        /* 
         * FIXME
         * Can be optimized by reading multiple sectors
         * or even entire blocks at one time
         */
        for (i=0; i<ctx->spb; i++) {
            bit = vhd_bitmap_test(ctx, bitmaps[0], i);

            if (bit)
                cur = ctx;
            else {
                level=0;
                list_for_each_entry(cur, &ctx->next, next) {
                    level++;
                    err = vhd_get_bat(cur);
                    if (err) 
                        goto out;

                    if (cur->bat.bat[block]==DD_BLK_UNUSED)
                        continue;

                    if(bitmaps[level]==NULL) {
                        err = vhd_read_bitmap(cur, 
                                block, &bitmaps[level]);
                        if (err) 
                            goto out;
                    }

                    bit = vhd_bitmap_test
                        (cur, bitmaps[level], i);

                    if (bit)
                        break;
                }
            }

            if (bit) {
                off  = vhd_sectors_to_bytes(blk + cur->bm_secs + i);

                err  = vhd_seek(cur, off, SEEK_SET);
                if (err) 
                    goto out;

                err = vhd_read(cur, 
                        buf + vhd_sectors_to_bytes(i), size);
                if (err) 
                    goto out;
            }
        }
    }
out:
    for(i=0; i<30; i++) 
        if(bitmaps[i])
            free(bitmaps[i]);

    return err;
}

static inline int
_vhd_util_copy2(const char *name, int fd) {

    int err = 0;
    vhd_context_t ctx;
    vhd_context_t **ctx_block_list = NULL;
    char *buf = NULL;
    uint32_t i = 0;
    int n;

    err = vhd_open(&ctx, name, VHD_OPEN_RDONLY | VHD_OPEN_CACHED);
    if (err) {
        fprintf(stderr, "failed to open %s: %s\n", 
                name, strerror(-err));
        return err;
    }

    err = vhd_get_bat(&ctx);
    if (err) {
        fprintf(stderr, "failed to get BAT for %s: %s\n", ctx.file,
                strerror(-err));
        goto out;
    }

    ctx_block_list = malloc(sizeof(vhd_context_t*) * ctx.bat.entries);
    if (!ctx_block_list) {
        fprintf(stderr, "failed to allocate memory\n");
        err = -ENOMEM;
        goto out;
    }

    err  = posix_memalign((void *)&buf, VHD_SECTOR_SIZE,
            vhd_sectors_to_bytes(ctx.spb));
    if (err) {
        err = -err;
        goto out;
    }

    err = vhd_get_used_blocks2(&ctx, ctx_block_list);
    if (err) {
        fprintf(stderr, "failed to get bat: %s\n", strerror(-err));
        goto out;
    }

    for (i = 0; i < ctx.bat.entries; i++) {
        if (ctx_block_list[i]) {
            err  = my_vhd_read_block(ctx_block_list[i], i, buf);
            if (err) {
                fprintf(stderr, 
                        "failed to read block %lu: %s\n",
                        (unsigned long)i, 
                        strerror(-err));
                goto out;
            }

            n = write(fd, buf, vhd_sectors_to_bytes(ctx.spb));
            if (n < 0) {
                fprintf(stderr, 
                        "failed to write block %lu: %s\n",
                        (unsigned long)i, 
                        strerror(-err));
                goto out;
            }
        }
    }
out:
    vhd_close(&ctx);
    free(ctx_block_list);
    free(buf);
    return err;
}

int
vhd_util_copy(const int argc, char **argv)
{
    char *name = NULL, *to = NULL;
    int c = 0, err = 0, fd = 0, stdo = 0;

    if (!argc || !argv)
        goto usage;

    while ((c = getopt(argc, argv, "n:t:sh")) != -1) {
        switch (c) {
            case 'n':
                name = optarg;
                break;
            case 't':
                to = optarg;
                break;
            case 's': 
                stdo = 1;
                break;
            case 'h':
            default:
                goto usage;
        }
    }

    if (!name || !(to || stdo)) {
        fprintf(stderr, 
                "missing source and/or target file/device name\n");
        goto usage;
    }

    if (name) { /* smart copy */
        if(stdo)
            fd = 1;
        else {
            fd = open(to, O_WRONLY | O_CREAT |
                    O_TRUNC | O_LARGEFILE | O_DIRECT, 0644);

            if (fd == -1) {
                err = -errno;
                fprintf(stderr, "failed to open %s: %s\n", 
                        to, strerror(-err));
                goto error;
            }
        }
        err = _vhd_util_copy2(name, fd);
    } 

error:
    if (fd > 0)
        close(fd);
    return err;

usage:
    printf("options: <-n source file name> [-t target file name] [-s stream output to stdout] [-h help]\n");
    return -EINVAL;
}

