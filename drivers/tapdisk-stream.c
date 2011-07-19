/*
 * Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>

#include "list.h"
#include "scheduler.h"
#include "tapdisk.h"
#include "tapdisk-server.h"
#include "tapdisk-disktype.h"

#define POLL_READ                        0
#define POLL_WRITE                       1

#define MIN(a, b)                        ((a) < (b) ? (a) : (b))
#define BUG(_cond)                       td_panic()
#define BUG_ON(_cond)                    if (unlikely(_cond)) { td_panic(); }

#define TD_STREAM_MAX_REQS               16
#define TD_STREAM_REQ_SIZE               (sysconf(_SC_PAGE_SIZE) * 32)

typedef struct tapdisk_stream_request td_stream_req_t;
typedef struct tapdisk_stream td_stream_t;

struct tapdisk_stream_request {
	void                            *buf;
	struct td_iovec                  iov;
	td_vbd_request_t                 vreq;
	struct list_head                 entry;
};

struct tapdisk_stream {
	td_vbd_t                        *vbd;

	unsigned int                     id;
	int                              in_fd;
	int                              out_fd;

	int                              err;

	td_sector_t                      sec_in;
	td_sector_t                      sec_out;
	uint64_t                         count;

	struct list_head                 pending_list;
	struct list_head                 completed_list;

	td_stream_req_t                  reqs[TD_STREAM_MAX_REQS];
	td_stream_req_t                 *free[TD_STREAM_MAX_REQS];
	int                              n_free;
};

static unsigned int tapdisk_stream_count;

static void tapdisk_stream_close_image(td_stream_t *);
static void tapdisk_stream_queue_requests(td_stream_t *);

static void
usage(const char *app, int err)
{
	printf("usage: %s <-n type:/path/to/image> "
	       "[-c sector count] [-s skip sectors]\n", app);
	exit(err);
}

static inline int
tapdisk_stream_stop(td_stream_t *s)
{
	return (list_empty(&s->pending_list) && (!s->count || s->err));
}

static int
tapdisk_stream_req_create(td_stream_req_t *req)
{
	int prot, flags;

	memset(req, 0, sizeof(*req));
	INIT_LIST_HEAD(&req->entry);

	prot  = PROT_READ|PROT_WRITE;
	flags = MAP_ANONYMOUS|MAP_PRIVATE;

	req->buf = mmap(NULL, TD_STREAM_REQ_SIZE, prot, flags, -1, 0);
	if (req->buf == MAP_FAILED) {
		req->buf = NULL;
		return -errno;
	}

	return 0;
}

static void
tapdisk_stream_req_destroy(td_stream_req_t *req)
{
	if (req->buf) {
		int err = munmap(req->iov.base, TD_STREAM_REQ_SIZE);
		BUG_ON(err);
		req->iov.base = NULL;
	}
}

td_stream_req_t *
tapdisk_stream_alloc_req(td_stream_t *s)
{
	td_stream_req_t *req = NULL;

	if (likely(s->n_free))
		req = s->free[--s->n_free];

	return req;
}

void
tapdisk_stream_free_req(td_stream_t *s, td_stream_req_t *req)
{
	BUG_ON(s->n_free >= MAX_REQUESTS);
	BUG_ON(!list_empty(&req->entry));
	s->free[s->n_free++] = req;
}

static void
tapdisk_stream_destroy_reqs(td_stream_t *s)
{
	td_stream_req_t *req;

	do {
		req = tapdisk_stream_alloc_req(s);
		if (!req)
			break;

		tapdisk_stream_req_destroy(req);
	} while (1);
}

static int
tapdisk_stream_create_reqs(td_stream_t *s)
{
	size_t size;
	void *buf;
	int i, err;

	s->n_free = 0;

	for (i = 0; i < TD_STREAM_MAX_REQS; i++) {
		td_stream_req_t *req = &s->reqs[i];

		err = tapdisk_stream_req_create(req);
		if (err)
			goto fail;

		tapdisk_stream_free_req(s, req);
	}

	return 0;

fail:
	tapdisk_stream_destroy_reqs(s);
	return err;
}

static int
tapdisk_stream_print_request(td_stream_t *s, td_stream_req_t *req)
{
	struct td_iovec *iov = &req->iov;

	int gcc = write(s->out_fd, iov->base, iov->secs << SECTOR_SHIFT);

	return iov->secs;
}

static void
tapdisk_stream_write_data(td_stream_t *s)
{
	td_stream_req_t *req, *next;

	list_for_each_entry_safe(req, next, &s->completed_list, entry) {
		if (req->vreq.sec != s->sec_out)
			break;

		s->sec_out += tapdisk_stream_print_request(s, req);

		list_del_init(&req->entry);
		tapdisk_stream_free_req(s, req);
	}
}

static inline void
tapdisk_stream_queue_completed(td_stream_t *s, td_stream_req_t *req)
{
	td_stream_req_t *itr;

	list_for_each_entry(itr, &s->completed_list, entry)
		if (req->vreq.sec < itr->vreq.sec)
			break;

	list_add_tail(&req->entry, &itr->entry);
}

static void
tapdisk_stream_complete_request(td_stream_t *s, td_stream_req_t *req, 
				int error, int final)
{
	list_del_init(&req->entry);

	if (likely(!error))
		tapdisk_stream_queue_completed(s, req);
	else {
		s->err = EIO;
		tapdisk_stream_free_req(s, req);
		fprintf(stderr, "error reading sector 0x%"PRIx64"\n",
			req->vreq.sec);
	}

	if (!final)
		return;

	tapdisk_stream_write_data(s);

	if (tapdisk_stream_stop(s)) {
		tapdisk_stream_close_image(s);
		return;
	}

	tapdisk_stream_queue_requests(s);
}

static void
__tapdisk_stream_request_cb(td_vbd_request_t *vreq, int error,
			    void *token, int final)
{
	td_stream_req_t *req = containerof(vreq, td_stream_req_t, vreq);
	td_stream_t *s = token;

	tapdisk_stream_complete_request(s, req, error, final);
}

static void
tapdisk_stream_queue_request(td_stream_t *s, td_stream_req_t *req)
{
	td_vbd_request_t *vreq;
	struct td_iovec *iov;
	int secs, err;

	iov   = &req->iov;
	secs  = MIN(TD_STREAM_REQ_SIZE >> SECTOR_SHIFT, s->count);

	iov->base           = req->buf;
	iov->secs           = secs;

	vreq                = &req->vreq;
	vreq->iov           = iov;
	vreq->iovcnt        = 1;
	vreq->sec           = s->sec_in;
	vreq->op            = TD_OP_READ;
	vreq->name          = NULL;
	vreq->token         = s;
	vreq->cb            = __tapdisk_stream_request_cb;

	s->count  -= secs;
	s->sec_in += secs;

	err = tapdisk_vbd_queue_request(s->vbd, vreq);
	if (err)
		tapdisk_stream_complete_request(s, req, err, 1);

	list_add_tail(&req->entry, &s->pending_list);
}

static void
tapdisk_stream_queue_requests(td_stream_t *s)
{

	while (s->count && !s->err) {
		td_stream_req_t *req;

		req = tapdisk_stream_alloc_req(s);
		if (!req)
			break;

		tapdisk_stream_queue_request(s, req);
	}
}

static int
tapdisk_stream_open_image(struct tapdisk_stream *s, const char *name)
{
	int err;

	s->id = tapdisk_stream_count++;

	err = tapdisk_server_initialize(NULL, NULL);
	if (err)
		goto out;

	err = tapdisk_vbd_initialize(-1, -1, s->id);
	if (err)
		goto out;

	s->vbd = tapdisk_server_get_vbd(s->id);
	if (!s->vbd) {
		err = ENODEV;
		goto out;
	}

	err = tapdisk_vbd_open_vdi(s->vbd, name, TD_OPEN_RDONLY, -1);
	if (err)
		goto out;

	err = 0;

out:
	if (err)
		fprintf(stderr, "failed to open %s: %d\n", name, err);
	return err;
}

static void
tapdisk_stream_close_image(td_stream_t *s)
{
	td_vbd_t *vbd;

	vbd = tapdisk_server_get_vbd(s->id);
	if (vbd) {
		tapdisk_vbd_close_vdi(vbd);
		tapdisk_server_remove_vbd(vbd);
		free(vbd->name);
		free(vbd);
		s->vbd = NULL;
	}
}

static int
tapdisk_stream_set_position(td_stream_t *s,
			    uint64_t count, uint64_t skip)
{
	int err;
	td_disk_info_t info;

	err = tapdisk_vbd_get_disk_info(s->vbd, &info);
	if (err) {
		fprintf(stderr, "failed getting image size: %d\n", err);
		return err;
	}

	if (count == -1LL)
		count = info.size - skip;

	if (count + skip > info.size) {
		fprintf(stderr, "0x%"PRIx64" past end of image 0x%"PRIx64"\n",
			count + skip, info.size);
		return -EINVAL;
	}

	s->sec_in  = skip;
	s->sec_out = skip;
	s->count   = count;

	return 0;
}

void
__tapdisk_stream_event_cb(event_id_t id, char mode, void *arg)
{
	td_stream_t *s = arg;
}

static int
tapdisk_stream_open_fds(struct tapdisk_stream *s)
{
	s->out_fd = dup(STDOUT_FILENO);
	if (s->out_fd == -1) {
		fprintf(stderr, "failed to open output: %d\n", errno);
		return errno;
	}

	return 0;
}

static void
tapdisk_stream_close(struct tapdisk_stream *s)
{
	tapdisk_stream_destroy_reqs(s);

	tapdisk_stream_close_image(s);

	if (s->out_fd >= 0) {
		close(s->out_fd);
		s->out_fd = -1;
	}
}

static int
tapdisk_stream_open(struct tapdisk_stream *s, const char *name,
		    uint64_t count, uint64_t skip)
{
	int err = 0;

	memset(s, 0, sizeof(*s));
	s->in_fd = s->out_fd = -1;
	INIT_LIST_HEAD(&s->pending_list);
	INIT_LIST_HEAD(&s->completed_list);

	if (!err)
		err = tapdisk_stream_open_fds(s);
	if (!err)
		err = tapdisk_stream_open_image(s, name);
	if (!err)
		err = tapdisk_stream_set_position(s, count, skip);
	if (!err)
		err = tapdisk_stream_create_reqs(s);

	if (err)
		tapdisk_stream_close(s);

	return err;
}

static int
tapdisk_stream_run(struct tapdisk_stream *s)
{
	tapdisk_stream_queue_requests(s);
	tapdisk_server_run();
	return s->err;
}

int
main(int argc, char *argv[])
{
	int c, err;
	const char *params;
	const char *path;
	uint64_t count, skip;
	struct tapdisk_stream stream;

	err    = 0;
	skip   = 0;
	count  = (uint64_t)-1;
	params = NULL;

	while ((c = getopt(argc, argv, "n:c:s:h")) != -1) {
		switch (c) {
		case 'n':
			params = optarg;
			break;
		case 'c':
			count = strtoull(optarg, NULL, 10);
			break;
		case 's':
			skip = strtoull(optarg, NULL, 10);
			break;
		default:
			err = EINVAL;
		case 'h':
			usage(argv[0], err);
		}
	}

	if (!params)
		usage(argv[0], EINVAL);

	tapdisk_start_logging("tapdisk-stream", "daemon");

	err = tapdisk_stream_open(&stream, params, count, skip);
	if (err)
		goto out;

	err = tapdisk_stream_run(&stream);
	if (err)
		goto out;

	err = 0;

out:
	tapdisk_stream_close(&stream);
	tapdisk_stop_logging();
	return err;
}
