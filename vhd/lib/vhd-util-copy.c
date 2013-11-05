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
#include <libaio.h>
#include <stdbool.h>

#include "libvhd.h"
#include "vhd-util.h"

#define BYTE_SHIFT 3

#ifdef DEBUG
#undef DEBUG
#define DEBUG 0
#endif

#if DEBUG == 1
#define DBG(_fmt, _args...) printf("%s:%d "_fmt, __FILE__, __LINE__, ##_args)
#else
#define DBG(_fmt, _args...) ;
#endif

#define list_last_entry(ptr, type, member) \
    list_entry((ptr)->prev, type, member)

/*
 * FIXME use caching
 */
struct iocb *alloc_iocb(void) {
    return calloc(1, sizeof(struct iocb));
}

void put_iocb(struct iocb *iocb) {
    free(iocb);
}

static int bitmap_size = 0;

/*
 * FIXME use caching
 */
int init_bitmap_cache(int n) {
    bitmap_size = n;
    return 0;
}

char *alloc_bitmap(void) {

    int err = 0;
    void *addr = NULL;

    assert(bitmap_size > 0);

    err = posix_memalign(&addr, VHD_SECTOR_SIZE, bitmap_size);
    if (err) {
        errno = err;
        return NULL;
    }
    bzero(addr, bitmap_size);
    return addr;
}

void put_bitmap(char *bitmap) {
    free(bitmap);
}

char *alloc_buf(int spb) {
    int err = 0;
    void *addr = NULL;
    int size = spb * VHD_SECTOR_SIZE;

    assert(bitmap_size > 0);

    err = posix_memalign(&addr, VHD_SECTOR_SIZE, size);
    if (err) {
        errno = err;
        return NULL;
    }
    bzero(addr, size);
    return addr;
}

void put_buf(char *buf) {
    free(buf);
}

enum io_type {MTDT, DATA};

/*
 * TODO change name, this struct is for a VHD block
 */
struct bitmap_desc {

    /**
     * File descriptor to write data to.
     */
    int fd;

    enum io_type io_type;

    /**
     * The leaf.
     */
    vhd_context_t *ctx;

    /**
     * The current VHD level.
     */
    vhd_context_t *cur_ctx;

    /**
     * The VHD block the bitmaps are for.
     */
    uint32_t block;

    /**
     * Pending sectors for which their bitmap has not yet been resolved.
     */
    unsigned long long sectors;

    /**
     * Sectors for which their bitmap has been resolved.
     */
    unsigned long long bmp_res_sect;

    /**
     * An array indexed by relative sector, pointing to the VHD context from
     * which we must read the sector data. A NULL value means that the sector
     * has never been written to.
     */
    vhd_context_t **ctxs;

    /**
     * The actual bitmap.
     */
    char *bmp;

    /**
     * iocb for metadata I/O
     */
    struct iocb iocb;
    io_context_t *aioctx;

    struct iocb **data_iocbs;
    unsigned int pending_data_iocbs;

    /**
     * The buffer containing the data.
     */
    char *buf;

    /*
     * pending iocbs
     */
    int pending;
};

/**
 * TODO use a cache
 */
struct bitmap_desc *alloc_bitmap_desc(void) {
    return calloc(1, sizeof(struct bitmap_desc));
}

void put_bitmap_desc(struct bitmap_desc *bmp) {
    if (bmp) {
        free(bmp->bmp);
        free(bmp->ctxs);
        if (bmp->data_iocbs) {
            int i;
            for (i = 0; i < bmp->ctx->spb && bmp->data_iocbs[i]; i++)
                put_iocb(bmp->data_iocbs[i]);
            free(bmp->data_iocbs);
        }
        free(bmp->buf);
        free(bmp);
    }
}

static int pending = 0;

/**
 * Submits all I/Os necessary for reading all data, using bmp->ctxs. If there
 * are no data to be read, @bmp is freed.
 */
static int
read_blocks(struct bitmap_desc *bmp) {
    int i;
    int err = 0;
    struct iocb *prv_iocb;
    vhd_context_t *prv_ctx;

    assert(bmp);

    bmp->data_iocbs = calloc(bmp->ctx->spb, sizeof(struct iocb*));
    if (!bmp->data_iocbs) {
        err = errno;
        goto out;
    }

    /*
     * FIXME If we're not reading an entire VHD block in total we're wasting
     * memory. However, it may be that we allocate memory more efficiently by
     * allocating/free fixed-size objects.
     */
    err = posix_memalign((void*)&bmp->buf, VHD_SECTOR_SIZE,
            VHD_SECTOR_SIZE * bmp->ctx->spb);
    if (err) {
        errno = err;
        goto out;
    }

    for (i = 0, bmp->pending_data_iocbs = 0, prv_iocb = NULL, prv_ctx = NULL;
            i < bmp->ctx->spb; i++) {
        if (bmp->ctxs[i]) {
            if (prv_ctx == bmp->ctxs[i]) {
                prv_iocb->u.c.nbytes += VHD_SECTOR_SIZE;
            } else {
                const unsigned long long off
                    = vhd_sectors_to_bytes(bmp->ctxs[i]->bat.bat[bmp->block]
                            + i + bmp->ctx->bm_secs);
                void *addr = bmp->buf + (i << VHD_SECTOR_SHIFT);

                /*
                 * FIXME need to handle resized VHD
                 */
                assert(bmp->ctxs[i]->bat.entries >= bmp->block);

                prv_iocb = bmp->data_iocbs[bmp->pending_data_iocbs]
                    = alloc_iocb();
                if (!prv_iocb) {
                    err = ENOMEM;
                    goto out;
                }

                io_prep_pread(prv_iocb, bmp->ctxs[i]->fd, addr,
                        VHD_SECTOR_SIZE, off);
                bmp->pending_data_iocbs++;
                prv_iocb->data = bmp;
                prv_ctx = bmp->ctxs[i];
            }
        } else {
            prv_ctx = NULL;
            prv_iocb = NULL;
        }
    }

    assert(bmp->pending_data_iocbs >= 0);

    if (bmp->pending_data_iocbs) {

        bmp->io_type = DATA;

        err = io_submit(*bmp->aioctx, bmp->pending_data_iocbs, bmp->data_iocbs);

        DBG("submitted %d I/Os for %u\n", bmp->pending_data_iocbs, bmp->block);

        if (err < 0) {
            err = -err;
            fprintf(stderr, "failed to submit data I/O for block %u: %s\n",
                    bmp->block, strerror(err));
        } else if (err != bmp->pending_data_iocbs) {
            fprintf(stderr, "submitted only %d iocbs out of %d\n", err,
                    bmp->pending_data_iocbs);
            err = EINVAL;
        } else {
            pending += bmp->pending_data_iocbs;
            err = 0;
        }
    } else
        put_bitmap_desc(bmp);
out:
    if (err) {
        if (bmp->data_iocbs) {
            /*
             * FIXME cannot free iocbs because some of them might have already
             * been submitted, attempt to cancel (io_cancel)?
             */
#if 0
            for (i = 0; i < bmp->ctx->spb && bmp->data_iocbs[i]; i++)
                put_iocb(bmp->data_iocbs[i]);
            free(bmp->buf);
#endif
            free(bmp->data_iocbs);
        }
    }
    return err;
}

/**
 * Returs 0 on success.
 *
 * If bmp->sectors is 0 after the call then the bitmaps have been populated
 * and data I/O can be initiated.
 */
int read_bitmap(struct bitmap_desc *bmp) {

    int err = 0;
    int bit;
    uint32_t block;

    assert(bmp);
    assert(bmp->sectors > 0);

    block = bmp->cur_ctx->bat.bat[bmp->block];
    if (block == DD_BLK_UNUSED) {
        DBG("block %u is unused at %s\n", bmp->block, bmp->cur_ctx->file);
        if (list_empty(&bmp->ctx->next) ||
                list_is_last(&bmp->cur_ctx->next, &bmp->ctx->next)) {

            DBG("%s is not part of a chain or %s is last in chain\n",
                    bmp->ctx->file, bmp->cur_ctx->file);
            /*
             * block not allocated at topmost VHD, we're done reading the
             * bit maps
             */
            bmp->sectors = 0;
            return read_blocks(bmp);
        } else {
            /*
             * proceed to the next level
             */
            bmp->cur_ctx = list_entry(bmp->cur_ctx->next.next, vhd_context_t,
                    next);
            return read_bitmap(bmp);
        }
    }

    DBG("block %u is used at %s\n", bmp->block, bmp->cur_ctx->file);

    err = -vhd_get_batmap(bmp->cur_ctx);
    if (err) {
        fprintf(stderr, "failed to get batmap for block %u on %s: %s\n",
                bmp->block, bmp->cur_ctx->file, strerror(err));
        return err;
    }

    /*
     * check the BATMAP first
     */
    bit = vhd_has_batmap(bmp->cur_ctx) && vhd_batmap_test(bmp->cur_ctx,
            &bmp->cur_ctx->batmap, bmp->block);

    DBG("BATMAP bit is %s for %u on %s\n", bit ? "ON": "OFF", bmp->block,
            bmp->cur_ctx->file);

    if (bit) {
        /*
         * If the bit in the BATMAP is set, set remaining sectors in the block
         * and finish.
         */
        int i;
        for (i = 0; i < bmp->cur_ctx->spb && bmp->sectors > 0; i++) {
            if (!bmp->ctxs[i]) {
                bmp->ctxs[i] = bmp->cur_ctx;
                bmp->sectors--;
            }
        }

        assert(bmp->sectors >= 0);

        if (!bmp->sectors)
            return read_blocks(bmp);
    } else {
        /**
         * read the bitmap
         */
        off64_t offset = vhd_sectors_to_bytes(block);
        struct iocb *p_iocb = &bmp->iocb;
        io_prep_pread(&bmp->iocb, bmp->cur_ctx->fd, bmp->bmp, bitmap_size,
                offset);
        bmp->iocb.data = bmp;
        err = io_submit(*bmp->aioctx, 1, &p_iocb);
        if (err < 0) {
            fprintf(stderr, "failed to submit I/O request: %s\n",
                    strerror(-err));
        } else {
            pending++;
            err = 0;
        }
    }
    return err;
}

static void
bitmap_read(struct bitmap_desc *bmp) {
    int i;

    assert(bmp);

    for (i = 0; i < bmp->ctx->spb && bmp->sectors > 0; i++) {
        if (vhd_bitmap_test(bmp->ctx, bmp->bmp, i)) {
            if (!bmp->ctxs[i]) {
                bmp->ctxs[i] = bmp->cur_ctx;
                bmp->sectors--;
            }
        }
    }
}

/**
 */
struct bitmap_desc* read_bitmaps(vhd_context_t *ctx, uint32_t block,
        io_context_t *aioctx, const int fd) {

    struct bitmap_desc *bmp = NULL;
    int err = 0;

    assert(ctx);
    assert(aioctx);

    bmp = alloc_bitmap_desc();
    if (!bmp) {
        err = errno;
        goto out;
    }

    bmp->block = block;
    bmp->cur_ctx = bmp->ctx = ctx;
    bmp->aioctx = aioctx;
    bmp->sectors = bmp->ctx->spb;
    bmp->fd = fd;

    bmp->ctxs = calloc(bmp->ctx->spb, sizeof(vhd_context_t*));
    if (!bmp->ctxs) {
        err = errno;
        goto out;
    }

    bmp->bmp = alloc_bitmap();
    if (!bmp->bmp) {
        err = errno;
        goto out;
    }

    /*
     * read the 1st bitmap
     */
    err = read_bitmap(bmp);

out:
    if (err) {
        put_bitmap_desc(bmp);
        bmp = NULL;
        errno = err;
    }

    return bmp;
}

static int
read_bat_chain(vhd_context_t *ctx) {
    int err = 0;
    vhd_context_t *cur = NULL;
    assert(ctx);
    list_for_each_entry(cur, &ctx->next, next) {
        err = vhd_get_bat(cur);
        if (err)
            return err;
    }
    return 0;
}

static int
process_mtdt_completion(struct bitmap_desc *bmp) {

    assert(bmp);

    bitmap_read(bmp);
    if (!bmp->sectors) {
        DBG("done reading bitmaps for %u\n", bmp->block);
    } else {
        /*
         * FIXME duplicated from read_bitmap
         */
        if (bmp->cur_ctx->bat.bat[bmp->block] != DD_BLK_UNUSED) {
            if (list_empty(&bmp->ctx->next) ||
                (bmp->ctx != bmp->cur_ctx
                 && list_is_last(&bmp->cur_ctx->next, &bmp->ctx->next))) {
                /*
                 * block not allocated at topmost VHD, we're done reading the
                 * bit maps
                 */
                bmp->sectors = 0;
                return read_blocks(bmp);
            } else {
                int err = 0;
                /*
                 * proceed to the next level
                 */
                bmp->cur_ctx = list_entry(bmp->cur_ctx->next.next,
                        vhd_context_t, next);
                err = read_bitmap(bmp);
                if (err) {
                    fprintf(stderr, "failed to read bitmap for block "
                            "%u: %s\n", bmp->block, strerror(err));
                    return err;
                }
                if (!bmp->sectors)
                    return 0;
            }
        } else
            bmp->sectors = 0;
    }

    /*
     * If we're done reading metadata, figure out which parts of which data
     * blocks need to be read and read them.
     */
    if (!bmp->sectors)
        return read_blocks(bmp);
    return 0;
}

static int
process_data_completion(struct bitmap_desc *bmp) {
    int err = 0;
    bmp->pending_data_iocbs--;

    assert(bmp->pending_data_iocbs >= 0);
    if (bmp->pending_data_iocbs == 0) {

        DBG("last data I/O completed for %u\n", bmp->block);

        if (bmp->fd) {
            const size_t blk_size = bmp->ctx->spb << VHD_SECTOR_SHIFT;
            const unsigned long long _off = bmp->block * blk_size;
            const off64_t off = lseek64(bmp->fd, _off, SEEK_SET);
            if (off == (off64_t) - 1) {
                err = errno;
                fprintf(stderr, "failed to seek output to %llu: %s\n", _off,
                        strerror(errno));
            } else {
                /**
                 * FIXME use libaio if we're writing to a file
                 */
                printf("%u %llu\n", bmp->block, off);
                ssize_t written = write(bmp->fd, bmp->buf, blk_size);
                if (written != blk_size) {
                    err = errno;
                    fprintf(stderr, "failed to write block %u "
                            "(written %u out of %u): %s\n", bmp->block,
                            written, blk_size, strerror(err));
                }
            }
        }

        /*
         * FIXME all data I/Os finished, we can now spit out the data.
         */
        put_bitmap_desc(bmp);
    } else {
        DBG("%d pending data I/Os for %u\n", bmp->pending_data_iocbs,
                bmp->block);
    }
    return err;
}

/*
 * an I/O request has completed
 */
static int
process_completion(struct io_event *io_event) {

    struct iocb *iocb = NULL;
    struct bitmap_desc *bmp = NULL;

    assert(io_event);

    iocb = io_event->obj;
    assert(iocb);

    pending--;

    bmp = (struct bitmap_desc*)iocb->data;
    assert(bmp);

    assert(bmp->io_type == MTDT || bmp->io_type == DATA);

    if (bmp->io_type == MTDT)
        return process_mtdt_completion(bmp);
    else
        return process_data_completion(bmp);
}

static inline int
_vhd_util_copy2(const char *name, int fd) {

    io_context_t aioctx;
    int max_events = 16384;
    vhd_context_t ctx;
    uint32_t blk;
    int err;
    struct io_event io_event;

    /*
     * FIXME allow user specify maxevents
     */
    bzero(&aioctx, sizeof(aioctx));
    err = io_queue_init(max_events, &aioctx);
    if (err) {
        err = errno;
        fprintf(stderr, "failed to initialise AIO context: %s\n",
                strerror(err));
        goto out;
    }

    err = vhd_open(&ctx, name, VHD_OPEN_RDONLY | VHD_OPEN_CACHED);
    if (err) {
        fprintf(stderr, "failed to open %s: %s\n",
                name, strerror(-err));
        goto out;
    }

    err = vhd_get_bat(&ctx);
    if (err) {
        fprintf(stderr, "failed to get BAT for %s: %s\n", ctx.file,
                strerror(-err));
        goto out;
    }

    err = read_bat_chain(&ctx);
    if (err)
        goto out;

    init_bitmap_cache(ctx.spb);

    for (blk = 0; blk < ctx.bat.entries; blk++) {
        struct bitmap_desc *bmp = read_bitmaps(&ctx, blk, &aioctx, fd);
        if (!bmp) {
            err = errno;
            fprintf(stderr, "failed to read bitmaps: %s\n", strerror(err));
            break;
        }

        err = io_getevents(aioctx, 0, 1, &io_event, NULL);
        if (err < 0) {
            err = -err;
            fprintf(stderr, "failed to get I/O events: %s\n",
                    strerror(err));
            break;
        } else if (err == 0) {
            /*
             * No I/O requests have completed, issue another one.
             */
            DBG("no I/Os have completed, proceeding to the next block\n");
            continue;
        } else {
            DBG("got an I/O completion\n");
            err = process_completion(&io_event);
            if (err)
                break;
        }
    }

    DBG("done issuing I/Os for bitmaps\n");

    while (pending) {
        DBG("waiting for an I/O completion (pending=%d)\n", pending);
        err = io_getevents(aioctx, 1, 1, &io_event, NULL);
        if (err < 0) {
            err = -err;
            fprintf(stderr, "failed to get I/O events: %s\n", strerror(err));
            break;
        } else if (err == 0 || err > 1) {
            fprintf(stderr, "unexpected number of completed I/O events: %d\n",
                    err);
            break;
        } else {
            DBG("got an I/O completion\n");
            assert(err > 0);
            err = process_completion(&io_event);
            if (err)
                break;
        }

    }

    if (fd) {
        /*
         * Write the EOF.
         */
        err = ftruncate(fd, (ctx.bat.entries * ctx.spb) << VHD_SECTOR_SHIFT);
        if (err == -1) {
            err = errno;
            fprintf(stderr, "failed to write EOF: %s\n", strerror(err));
        }

        err = close(fd);
        if (err == -1) {
            err = errno;
            fprintf(stderr, "failed to close output: %s\n", strerror(err));
        }
    }

    /*
     * FIXME close vhd, put BATs, ...
     */
out:
    return err;
}

int
vhd_util_copy(const int argc, char **argv)
{
    char *name = NULL, *to = NULL;
    int c = 0, err = 0, fd = -1, stdo = 0;

    if (!argc || !argv)
        goto usage;

    while ((c = getopt(argc, argv, "n:o:sh")) != -1) {
        switch (c) {
            case 'n':
                name = optarg;
                break;
            case 'o':
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

    if (!name || !(!!to ^ !!stdo)) {
        fprintf(stderr,
                "missing source and/or target file/device name\n");
        goto usage;
    }

    if (name) { /* smart copy */
        if (stdo)
            fd = fileno(stdout);
        else  if (to) {
            int flags = O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE;
            if (strcmp("/dev/null", to))
                flags |= O_DIRECT;
            fd = open(to, flags, 0644);

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
    printf("options: <-n source file name> [-o target file name] [-s stream output to stdout] [-h help]\n");
    return -EINVAL;
}
