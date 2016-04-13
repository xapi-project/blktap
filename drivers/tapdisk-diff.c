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

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "list.h"
#include "scheduler.h"
#include "tapdisk-vbd.h"
#include "tapdisk-server.h"
#include "tapdisk-disktype.h"
#include "timeout-math.h"
#include "libvhd.h"

#define POLL_READ                        0
#define POLL_WRITE                       1

#define SPB_SHIFT (VHD_BLOCK_SHIFT - SECTOR_SHIFT)

/* 
 * we have to use half the max number of requests because we're using the same 
 * tapdisk server for both streams and all the parents will be shared. If we 
 * issue more than MAX_REQUESTS/2 requests, the vhd_state will run out of 
 * vhd_request's and return EBUSY, which we don't handle here. However, even 
 * with MAX_REQUESTS/2 we can still run out of vhd_request's because of 
 * splitting: if some sectors spanned by a segment are in a parent, a segment 
 * could be split into at most N/2 vhd_request's, where N is the number of 
 * sectors per segment. Therefore, if we use 11 segments, we need to divide 
 * MAX_REQUESTS by 11/2=6 on top of that. If we don't, we'd have to handle 
 * EBUSY by retrying here.
 */
#define MAX_SEGMENTS 8
#define MAX_STREAM_REQUESTS (MAX_REQUESTS / 2 / (MAX_SEGMENTS / 2))

struct tapdisk_stream_poll {
	int                              pipe[2];
	int                              set;
};

struct tapdisk_stream_request {
	uint64_t                         sec;
	uint32_t                         secs;
	uint64_t                         seqno;
	blkif_request_t                  blkif_req;
	struct list_head                 next;
};

struct tapdisk_stream {
	td_vbd_t                        *vbd;

	unsigned int                     id;

	int                              err;

	uint64_t                         cur;
	uint64_t                         start;
	uint64_t                         end;

	uint64_t                         started;
	uint64_t                         completed;

	struct tapdisk_stream_poll       poll;
	event_id_t                       enqueue_event_id;

	struct list_head                 free_list;
	struct list_head                 pending_list;
	struct list_head                 completed_list;

	struct tapdisk_stream_request    requests[MAX_STREAM_REQUESTS];
};

static unsigned int tapdisk_stream_count;

static void tapdisk_stream_close_image(struct tapdisk_stream *);

static char *program;
static struct tapdisk_stream stream1, stream2;
static vhd_context_t vhd1;

static void
usage(FILE *stream)
{
	printf("usage: %s <-n type:/path/to/image> <-m type:/path/to/image>\n",
			program);
}

static int
open_vhd(const char *path, vhd_context_t *vhd)
{
	int err;

	err = vhd_open(vhd, path, VHD_OPEN_RDONLY);
	if (err) {
		printf("error opening %s: %d\n", path, err);
		return err;
	}

	err = vhd_get_bat(vhd);
	if (err)
	{
		printf("error reading BAT for %s: %d\n", path, err);
		vhd_close(vhd);
		return err;
	}

	return 0;
}

static inline void
tapdisk_stream_poll_initialize(struct tapdisk_stream_poll *p)
{
	p->set = 0;
	p->pipe[POLL_READ] = p->pipe[POLL_WRITE] = -1;
}

static int
tapdisk_stream_poll_open(struct tapdisk_stream_poll *p)
{
	int err;

	tapdisk_stream_poll_initialize(p);

	err = pipe(p->pipe);
	if (err)
		return -errno;

	err = fcntl(p->pipe[POLL_READ], F_SETFL, O_NONBLOCK);
	if (err)
		goto out;

	err = fcntl(p->pipe[POLL_WRITE], F_SETFL, O_NONBLOCK);
	if (err)
		goto out;

	return 0;

out:
	close(p->pipe[POLL_READ]);
	close(p->pipe[POLL_WRITE]);
	tapdisk_stream_poll_initialize(p);
	return -errno;
}

static void
tapdisk_stream_poll_close(struct tapdisk_stream_poll *p)
{
	if (p->pipe[POLL_READ] != -1)
		close(p->pipe[POLL_READ]);
	if (p->pipe[POLL_WRITE] != -1)
		close(p->pipe[POLL_WRITE]);
	tapdisk_stream_poll_initialize(p);
}

static inline void
tapdisk_stream_poll_clear(struct tapdisk_stream_poll *p)
{
	int gcc, dummy;

	gcc = read(p->pipe[POLL_READ], &dummy, sizeof(dummy));
	p->set = 0;
}

static inline void
tapdisk_stream_poll_set(struct tapdisk_stream_poll *p)
{
	int dummy = 0;

	if (!p->set) {
		int gcc = write(p->pipe[POLL_WRITE], &dummy, sizeof(dummy));
		p->set = 1;
	}
}

static inline int
tapdisk_stream_stop(struct tapdisk_stream *s)
{
	return ((s->cur == s->end || s->err) &&
			list_empty(&s->pending_list) && 
			list_empty(&s->completed_list));
}

static inline void
tapdisk_stream_initialize_request(struct tapdisk_stream_request *req)
{
	memset(req, 0, sizeof(*req));
	INIT_LIST_HEAD(&req->next);
}

static inline int
tapdisk_stream_request_idx(struct tapdisk_stream *s,
			   struct tapdisk_stream_request *req)
{
	return (req - s->requests);
}

static inline struct tapdisk_stream_request *
tapdisk_stream_get_request(struct tapdisk_stream *s)
{
	struct tapdisk_stream_request *req;

	if (list_empty(&s->free_list))
		return NULL;

	req = list_entry(s->free_list.next,
			 struct tapdisk_stream_request, next);

	list_del_init(&req->next);
	tapdisk_stream_initialize_request(req);

	return req;
}

static inline void
tapdisk_stream_queue_completed(struct tapdisk_stream *s,
			       struct tapdisk_stream_request *sreq)
{
	struct tapdisk_stream_request *itr;

	list_for_each_entry(itr, &s->completed_list, next)
		if (sreq->seqno < itr->seqno) {
			list_add_tail(&sreq->next, &itr->next);
			return;
		}

	list_add_tail(&sreq->next, &s->completed_list);
}

static int 
tapdisk_result_compare(struct tapdisk_stream_request *sreq1,
		struct tapdisk_stream_request  *sreq2)
{
	unsigned long idx1, idx2;
	char *buf1, *buf2;
	int result;

	assert(sreq1->seqno == sreq2->seqno);
	assert(sreq1->secs == sreq2->secs);
	idx1 = (unsigned long)tapdisk_stream_request_idx(&stream1, 
			sreq1);
	idx2 = (unsigned long)tapdisk_stream_request_idx(&stream2,
			sreq2);
	buf1 = (char *)MMAP_VADDR(stream1.vbd->ring.vstart, idx1, 0);
	buf2 = (char *)MMAP_VADDR(stream2.vbd->ring.vstart, idx2, 0);

	result = memcmp(buf1, buf2, sreq1->secs << SECTOR_SHIFT);
	return result;
}

static int
tapdisk_stream_process_data(void)
{
	struct tapdisk_stream_request *sreq1, *sreq2, *tmp1, *tmp2;
	int advance_both;
	int result = 0;

	sreq1 = list_entry(stream1.completed_list.next,
			struct tapdisk_stream_request, next);
	sreq2 = list_entry(stream2.completed_list.next,
			struct tapdisk_stream_request, next);
	tmp1 = list_entry(sreq1->next.next,
			struct tapdisk_stream_request, next);
	tmp2 = list_entry(sreq2->next.next,
			struct tapdisk_stream_request, next);
	while (result == 0 &&
			&sreq1->next != &stream1.completed_list &&
			&sreq2->next != &stream2.completed_list) {
		//printf("checking: %llu|%llu\n", sreq1->seqno, sreq2->seqno);
		advance_both = 1;
		if (sreq1->seqno < sreq2->seqno) {
			advance_both = 0;
			goto advance1;
		}
		if (sreq1->seqno > sreq2->seqno)
			goto advance2;

		result = tapdisk_result_compare(sreq1, sreq2);

		stream1.completed++;
		stream2.completed++;
		
		list_del_init(&sreq1->next);
		list_add_tail(&sreq1->next, &stream1.free_list);
		list_del_init(&sreq2->next);
		list_add_tail(&sreq2->next, &stream2.free_list);

advance1:
		sreq1 = tmp1;
		tmp1 = list_entry(tmp1->next.next, 
				struct tapdisk_stream_request, next);
		if (!advance_both)
			continue;
advance2:
		sreq2 = tmp2;
		tmp2 = list_entry(tmp2->next.next, 
				struct tapdisk_stream_request, next);
	}

	return result;
}

static void
tapdisk_stream_dequeue(void *arg, blkif_response_t *rsp)
{
	struct tapdisk_stream *s = (struct tapdisk_stream *)arg;
	struct tapdisk_stream_request *sreq = s->requests + rsp->id;

	list_del_init(&sreq->next);

	if (rsp->status == BLKIF_RSP_OKAY)
		tapdisk_stream_queue_completed(s, sreq);
	else {
		s->err = EIO;
		list_add_tail(&sreq->next, &s->free_list);
		fprintf(stderr, "error reading sector %llu (stream %d)\n",
				sreq->sec, (s == &stream2) + 1);
	}

	if (tapdisk_stream_process_data()) {
		fprintf(stderr, "mismatch at sector %llu\n",
				sreq->sec);
		stream1.err = EINVAL;
		stream2.err = EINVAL;
	}

	tapdisk_stream_poll_set(&stream1.poll);
	tapdisk_stream_poll_set(&stream2.poll);
}

static inline int
tapdisk_stream_enqueue_copy(struct tapdisk_stream *s, 
		struct tapdisk_stream_request *r)
{
	td_vbd_t *vbd;
	blkif_request_t *breq;
	td_vbd_request_t *vreq;
	struct tapdisk_stream_request *sreq;
	int idx;

	vbd = stream2.vbd;
	sreq = tapdisk_stream_get_request(s);
	if (!sreq)
		return 1;

	idx                 = tapdisk_stream_request_idx(s, sreq);

	sreq->sec           = r->sec;
	sreq->secs          = r->secs;
	sreq->seqno         = r->seqno;

	breq                = &sreq->blkif_req;
	breq->id            = idx;
	breq->nr_segments   = r->blkif_req.nr_segments;
	breq->sector_number = r->blkif_req.sector_number;
	breq->operation     = BLKIF_OP_READ;

	for (int i = 0; i < r->blkif_req.nr_segments; i++) {
		struct blkif_request_segment *seg = breq->seg + i;
		seg->first_sect = r->blkif_req.seg[i].first_sect;
		seg->last_sect  = r->blkif_req.seg[i].last_sect;
	}
	s->cur += sreq->secs;

	vreq = vbd->request_list + idx;
	assert(list_empty(&vreq->next));
	assert(vreq->secs_pending == 0);

	memcpy(&vreq->req, breq, sizeof(*breq));
	s->started++;
	vbd->received++;
	vreq->vbd = vbd;

	tapdisk_vbd_move_request(vreq, &vbd->new_requests);
	list_add_tail(&sreq->next, &s->pending_list);

	return 0;
}

static void
tapdisk_stream_enqueue1(void)
{
	td_vbd_t *vbd;
	int i, idx, psize, blk;
	struct tapdisk_stream *s = &stream1;

	vbd = s->vbd;
	psize = getpagesize();

	while (s->cur < s->end && !s->err) {
		blkif_request_t *breq;
		td_vbd_request_t *vreq;
		struct tapdisk_stream_request *sreq;

		/* skip any blocks that are not present in this image */
		blk = s->cur >> SPB_SHIFT;
		while (s->cur < s->end && vhd1.bat.bat[blk] == DD_BLK_UNUSED) {
			//printf("skipping block %d\n", blk);
			blk++;
			s->cur = blk << SPB_SHIFT;
		}

		if (s->cur >= s->end)
			break;

		sreq = tapdisk_stream_get_request(s);
		if (!sreq)
			break;

		idx                 = tapdisk_stream_request_idx(s, sreq);

		sreq->sec           = s->cur;
		sreq->secs          = 0;
		sreq->seqno         = s->started++;

		breq                = &sreq->blkif_req;
		breq->id            = idx;
		breq->nr_segments   = 0;
		breq->sector_number = sreq->sec;
		breq->operation     = BLKIF_OP_READ;

		for (i = 0; i < MAX_SEGMENTS; i++) {
			uint32_t secs;
			struct blkif_request_segment *seg = breq->seg + i;

			secs = MIN(s->end - s->cur, psize >> SECTOR_SHIFT);
			secs = MIN(((blk + 1) << SPB_SHIFT) - s->cur, secs);
			if (!secs)
				break;

			sreq->secs += secs;
			s->cur     += secs;

			seg->first_sect = 0;
			seg->last_sect  = secs - 1;
			breq->nr_segments++;
		}

		vreq = vbd->request_list + idx;

		assert(list_empty(&vreq->next));
		assert(vreq->secs_pending == 0);

		memcpy(&vreq->req, breq, sizeof(*breq));
		vbd->received++;
		vreq->vbd = vbd;

		tapdisk_vbd_move_request(vreq, &vbd->new_requests);
		list_add_tail(&sreq->next, &s->pending_list);
	}

	tapdisk_vbd_issue_requests(vbd);
}

static void
tapdisk_stream_enqueue2(void)
{
	td_vbd_t *vbd;
	int i, blk;
	struct tapdisk_stream_request *itr;
	struct tapdisk_stream *s = &stream2;

	vbd = s->vbd;

	/* issue the same requests that we issued on stream1 */
	list_for_each_entry(itr, &stream1.completed_list, next) {
		if (itr->sec < s->cur)
			continue;
		if (tapdisk_stream_enqueue_copy(s, itr))
			goto done;
	}

	list_for_each_entry(itr, &stream1.pending_list, next) {
		if (itr->sec < s->cur)
			continue;
		if (tapdisk_stream_enqueue_copy(s, itr))
			goto done;
	}

	stream2.cur = stream1.cur;

done:
	tapdisk_vbd_issue_requests(vbd);
}

static inline int
tapdisk_diff_done(void)
{
	return (tapdisk_stream_stop(&stream1) && tapdisk_stream_stop(&stream2));
}

static void
tapdisk_diff_stop(void)
{
	tapdisk_stream_close_image(&stream1);
	tapdisk_stream_close_image(&stream2);
}

static void
tapdisk_stream_enqueue(event_id_t id, char mode, void *arg)
{
	struct tapdisk_stream *s = (struct tapdisk_stream *)arg;

	tapdisk_stream_poll_clear(&s->poll);

	if (tapdisk_diff_done()) {
		tapdisk_diff_stop();
		return;
	}

	if (s == &stream1) 
		tapdisk_stream_enqueue1();
	else if (s == &stream2)
		tapdisk_stream_enqueue2();
	else
		assert(0);

	if (tapdisk_diff_done()) {
		// we have to check again for the case when stream1 had no 
		// blocks at all
		tapdisk_diff_stop();
		return;
	}
}

static int
tapdisk_stream_open_image(struct tapdisk_stream *s, const char *name)
{
	int err;
	td_disk_info_t info;

	s->id = tapdisk_stream_count++;

	err = tapdisk_vbd_initialize(-1, -1, s->id);
	if (err)
		goto out;

	s->vbd = tapdisk_server_get_vbd(s->id);
	if (!s->vbd) {
		err = ENODEV;
		goto out;
	}

	tapdisk_vbd_set_callback(s->vbd, tapdisk_stream_dequeue, s);

	err = tapdisk_vbd_open_vdi(s->vbd, name, TD_OPEN_RDONLY, -1);
	if (err)
		goto out;

	err = tapdisk_vbd_get_disk_info(s->vbd, &info);
	if (err) {
		fprintf(stderr, "failed getting image size: %d\n", err);
		return err;
	}

	s->start = 0;
	s->cur   = s->start;
	s->end   = info.size;

	err = 0;

out:
	if (err)
		fprintf(stderr, "failed to open image %s: %d\n", name, err);
	return err;
}

static void
tapdisk_stream_close_image(struct tapdisk_stream *s)
{
	td_vbd_t *vbd;

	vbd = tapdisk_server_get_vbd(s->id);
	if (vbd) {
		tapdisk_vbd_close_vdi(vbd);
		tapdisk_server_remove_vbd(vbd);
		free((void *)vbd->ring.vstart);
		free(vbd->name);
		free(vbd);
		s->vbd = NULL;
	}
}

static int
tapdisk_stream_initialize_requests(struct tapdisk_stream *s)
{
	size_t size;
	td_ring_t *ring;
	int err, i, psize;

	ring  = &s->vbd->ring;
	psize = getpagesize();
	size  = psize * BLKTAP_MMAP_REGION_SIZE;

	/* sneaky -- set up ring->vstart so tapdisk_vbd will use our buffers */
	err = posix_memalign((void **)&ring->vstart, psize, size);
	if (err) {
		fprintf(stderr, "failed to allocate buffers: %d\n", err);
		ring->vstart = 0;
		return err;
	}

	for (i = 0; i < MAX_STREAM_REQUESTS; i++) {
		struct tapdisk_stream_request *req = s->requests + i;
		tapdisk_stream_initialize_request(req);
		list_add_tail(&req->next, &s->free_list);
	}

	return 0;
}

static int
tapdisk_stream_register_enqueue_event(struct tapdisk_stream *s)
{
	int err;
	struct tapdisk_stream_poll *p = &s->poll;

	err = tapdisk_stream_poll_open(p);
	if (err)
		goto out;

	err = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					    p->pipe[POLL_READ], TV_ZERO,
					    tapdisk_stream_enqueue, s);
	if (err < 0)
		goto out;

	s->enqueue_event_id = err;
	err = 0;

out:
	if (err)
		fprintf(stderr, "failed to register event: %d\n", err);
	return err;
}

static void
tapdisk_stream_unregister_enqueue_event(struct tapdisk_stream *s)
{
	if (s->enqueue_event_id) {
		tapdisk_server_unregister_event(s->enqueue_event_id);
		s->enqueue_event_id = 0;
	}
	tapdisk_stream_poll_close(&s->poll);
}

static inline void
tapdisk_stream_initialize(struct tapdisk_stream *s)
{
	memset(s, 0, sizeof(*s));
	INIT_LIST_HEAD(&s->free_list);
	INIT_LIST_HEAD(&s->pending_list);
	INIT_LIST_HEAD(&s->completed_list);
}

static int
tapdisk_stream_open(struct tapdisk_stream *s, const char *arg)
{
	int err;

	tapdisk_stream_initialize(s);

	err = tapdisk_stream_open_image(s, arg);
	if (err)
		return err;

	err = tapdisk_stream_initialize_requests(s);
	if (err)
		return err;

	err = tapdisk_stream_register_enqueue_event(s);
	if (err)
		return err;

	tapdisk_stream_enqueue(s->enqueue_event_id, 
			       SCHEDULER_POLL_READ_FD, s);

	return 0;
}

static void
tapdisk_stream_release(struct tapdisk_stream *s)
{
	tapdisk_stream_close_image(s);
	tapdisk_stream_unregister_enqueue_event(s);
}

static int
tapdisk_stream_run(struct tapdisk_stream *s)
{
	tapdisk_stream_enqueue(s->enqueue_event_id, SCHEDULER_POLL_READ_FD, s);
	tapdisk_server_run();
	return s->err;
}

int
main(int argc, char *argv[])
{
	int c, err, type1;
	const char *arg1 = NULL, *arg2 = NULL;
	const disk_info_t *info;
	const char *path1;

	err    = 0;

	program = basename(argv[0]);
	
	while ((c = getopt(argc, argv, "n:m:h")) != -1) {
		switch (c) {
		case 'n':
			arg1 = optarg;
			break;
		case 'm':
			arg2 = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			goto fail_usage;
		}
	}

	if (!arg1 || !arg2)
		goto fail_usage;

	type1 = tapdisk_disktype_parse_params(arg1, &path1);
	if (type1 < 0)
		return type1;

	if (type1 != DISK_TYPE_VHD) {
		printf("error: first VDI is not VHD\n");
		return EINVAL;
	}

	err = open_vhd(path1, &vhd1);
	if (err)
		return err;

	tapdisk_start_logging("tapdisk-diff", "daemon");

	err = tapdisk_server_initialize(NULL, NULL);
	if (err)
		goto out;

	err = tapdisk_stream_open(&stream1, arg1);
	if (err) {
		fprintf(stderr, "Failed to open %s: %s\n", 
			arg1, strerror(-err));
		goto out;
	}

	err = tapdisk_stream_open(&stream2, arg2);
	if (err) {
		fprintf(stderr, "Failed to open %s: %s\n", 
			arg2, strerror(-err));
		goto out1;
	}

	if (stream1.end != stream2.end) {
		fprintf(stderr, "Image sizes differ: %"PRIu64" != %"PRIu64"\n",
				stream1.end, stream2.end);
		err = EINVAL;
		goto out2;
	}

	tapdisk_server_run();
	
out2:
	tapdisk_stream_release(&stream2);
out1:
	tapdisk_stream_release(&stream1);
out:
	vhd_close(&vhd1);
	tapdisk_stop_logging();

	return err ? : stream1.err;

fail_usage:
	usage(stderr);
	return 1;
}
