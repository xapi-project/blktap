/*
 * Copyright (c) 2020, Citrix Systems, Inc.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <libaio.h>
#include <sys/eventfd.h>
#ifdef __linux__
#include <linux/version.h>
#endif

#include "tapdisk.h"
#include "tapdisk-log.h"
#include "libaio-backend.h"
#include "tapdisk-server.h"
#include "tapdisk-utils.h"
#include "timeout-math.h"

#include "atomicio.h"
#include "aio_getevents.h"

#define WARN(_f, _a...) tlog_write(TLOG_WARN, _f, ##_a)
#define DBG(_f, _a...) tlog_write(TLOG_DBG, _f, ##_a)
#define ERR(_err, _f, _a...) tlog_error(_err, _f, ##_a)

#define libaio_backend_queue_count(q) ((q)->queued)
#define libaio_backend_queue_empty(q) ((q)->queued == 0)
#define libaio_backend_queue_full(q)  \
	(((q)->tiocbs_pending + (q)->queued) >= (q)->size)
typedef struct _libaio_queue {
	int                   size;

	const struct tio     *tio;
	void                 *tio_data;

	struct opioctx        opioctx;

	int                   queued;
	struct iocb         **iocbs;

	/* number of iocbs pending in the aio layer */
	int                   iocbs_pending;

	/* number of tiocbs pending in the queue -- 
	 * this is likely to be larger than iocbs_pending 
	 * due to request coalescing */
	int                   tiocbs_pending;

	/* iocbs may be deferred if the aio ring is full.
	 * libaio_backend_queue_complete will ensure deferred
	 * iocbs are queued as slots become available. */
	struct tlist          deferred;
	int                   tiocbs_deferred;

	/* optional tapdisk filter */
	struct tfilter       *filter;

	uint64_t              deferrals;
} libaio_queue;

struct tio {
	const char           *name;
	size_t                data_size;

	int  (*tio_setup)    (libaio_queue *queue, int qlen);
	void (*tio_destroy)  (libaio_queue *queue);
	int  (*tio_submit)   (libaio_queue *queue);
};


/*
 * We used a kernel patch to return an fd associated with the AIO context
 * so that we can concurrently poll on synchronous and async descriptors.
 * This is signalled by passing 1 as the io context to io_setup.
 */
#define REQUEST_ASYNC_FD ((io_context_t)1)

static inline void
queue_tiocb(libaio_queue *queue, struct tiocb *tiocb)
{
	struct iocb *iocb = (struct iocb*)tiocb->iocb;

	if (queue->queued) {
		struct tiocb *prev = (struct tiocb *)
			queue->iocbs[queue->queued - 1]->data;
		prev->next = tiocb;
	}

	queue->iocbs[queue->queued++] = iocb;
}

static inline int
deferred_tiocbs(libaio_queue *queue)
{
	return (queue->deferred.head != NULL);
}

static inline void
defer_tiocb(libaio_queue *queue, struct tiocb *tiocb)
{
	struct tlist *list = &queue->deferred;

	if (!list->head)
		list->head = list->tail = tiocb;
	else
		list->tail = list->tail->next = tiocb;

	queue->tiocbs_deferred++;
	queue->deferrals++;
}

static inline void
queue_deferred_tiocb(libaio_queue *queue)
{
	struct tlist *list = &queue->deferred;

	if (list->head) {
		struct tiocb *tiocb = list->head;

		list->head = tiocb->next;
		if (!list->head)
			list->tail = NULL;

		queue_tiocb(queue, tiocb);
		queue->tiocbs_deferred--;
	}
}

static inline void
queue_deferred_tiocbs(libaio_queue *queue)
{
	while (!libaio_backend_queue_full(queue) && deferred_tiocbs(queue))
		queue_deferred_tiocb(queue);
}

/*
 * td_complete may queue more tiocbs
 */
static void
complete_tiocb(libaio_queue *queue, struct tiocb *tiocb, unsigned long res)
{
	int err;
	struct iocb *iocb = (struct iocb*)tiocb->iocb;

	if (res == iocb_nbytes(iocb))
		err = 0;
	else if ((int)res < 0)
		err = (int)res;
	else
		err = -EIO;

	tiocb->cb(tiocb->arg, tiocb, err);
	free(iocb);
}

static int
cancel_tiocbs(libaio_queue *queue, int err)
{
	int queued;
	struct tiocb *tiocb;

	if (!queue->queued)
		return 0;

	/* 
	 * td_complete may queue more tiocbs, which
	 * will overwrite the contents of queue->iocbs.
	 * use a private linked list to keep track
	 * of the tiocbs we're cancelling. 
	 */
	tiocb  = queue->iocbs[0]->data;
	queued = queue->queued;
	queue->queued = 0;

	for (; tiocb != NULL; tiocb = tiocb->next)
		complete_tiocb(queue, tiocb, err);

	return queued;
}

static int
fail_tiocbs(libaio_queue *queue, int succeeded, int total, int err)
{
	ERR(err, "io_submit error: %d of %d failed",
	    total - succeeded, total);

	/* take any non-submitted, merged iocbs 
	 * off of the queue, split them, and fail them */
	queue->queued = io_expand_iocbs(&queue->opioctx,
					queue->iocbs, succeeded, total);

	return cancel_tiocbs(queue, err);
}

/*
 * libaio
 */

struct lio {
	io_context_t     aio_ctx;
	struct io_event *aio_events;

	int              event_fd;
	int              event_id;

	int              flags;
};

#define LIO_FLAG_EVENTFD        (1<<0)

static int
libaio_backend_lio_check_resfd(void)
{
	return tapdisk_linux_version() >= KERNEL_VERSION(2, 6, 22);
}

static void
libaio_backend_lio_destroy_aio(libaio_queue *queue)
{
	struct lio *lio = queue->tio_data;

	if (lio->event_fd >= 0) {
		close(lio->event_fd);
		lio->event_fd = -1;
	}

	if (lio->aio_ctx) {
		io_destroy(lio->aio_ctx);
		lio->aio_ctx = 0;
	}
}

static int
__lio_setup_aio_poll(libaio_queue *queue, int qlen)
{
	struct lio *lio = queue->tio_data;
	int err, fd;

	lio->aio_ctx = REQUEST_ASYNC_FD;

	fd = io_setup(qlen, &lio->aio_ctx);
	if (fd < 0) {
		lio->aio_ctx = 0;
		err = -errno;

		if (err == -EINVAL)
			goto fail_fd;

		goto fail;
	}

	lio->event_fd = fd;

	return 0;

fail_fd:
	DPRINTF("Couldn't get fd for AIO poll support. This is probably "
		"because your kernel does not have the aio-poll patch "
		"applied.\n");
fail:
	return err;
}

static int
__lio_setup_aio_eventfd(libaio_queue *queue, int qlen)
{
	struct lio *lio = queue->tio_data;
	int err;

	err = io_setup(qlen, &lio->aio_ctx);
	if (err < 0) {
		lio->aio_ctx = 0;
		return err;
	}

	lio->event_fd = eventfd(0, 0);
	if (lio->event_fd < 0)
		return  -errno;

	lio->flags |= LIO_FLAG_EVENTFD;

	return 0;
}

static int
libaio_backend_lio_setup_aio(libaio_queue *queue, int qlen)
{
	struct lio *lio = queue->tio_data;
	int err, old_err = 0;

	lio->aio_ctx  =  0;
	lio->event_fd = -1;

	/*
	 * prefer the mainline eventfd(2) api, if available.
	 * if not, fall back to the poll fd patch.
	 */

	err = !libaio_backend_lio_check_resfd();
	if (!err)
		err = old_err = __lio_setup_aio_eventfd(queue, qlen);
	if (err)
		err = __lio_setup_aio_poll(queue, qlen);

	/* __lio_setup_aio_poll seems to always fail with EINVAL on newer systems,
	 * probably because it initializes the output parameter of io_setup to a
	 * non-zero value and the kernel patch that understands this is missing */
	if (err == -EAGAIN || (err && old_err == -EAGAIN))
		goto fail_rsv;
fail:
	return err;

fail_rsv:
	EPRINTF("Couldn't setup AIO context. If you are trying to "
		"concurrently use a large number of blktap-based disks, you may "
		"need to increase the system-wide aio request limit. "
		"(e.g. 'echo 1048576 > /proc/sys/fs/aio-max-nr')\n");
	goto fail;
}


static void
libaio_backend_lio_destroy(libaio_queue *queue)
{
	struct lio *lio = queue->tio_data;

	if (!lio)
		return;

	if (lio->event_id >= 0) {
		tapdisk_server_unregister_event(lio->event_id);
		lio->event_id = -1;
	}

	libaio_backend_lio_destroy_aio(queue);

	if (lio->aio_events) {
		free(lio->aio_events);
		lio->aio_events = NULL;
	}
}

static void
libaio_backend_lio_set_eventfd(libaio_queue *queue, int n, struct iocb **iocbs)
{
	struct lio *lio = queue->tio_data;
	int i;

	if (lio->flags & LIO_FLAG_EVENTFD)
		for (i = 0; i < n; ++i)
			io_set_eventfd(iocbs[i], lio->event_fd);
}

static void
libaio_backend_lio_ack_event(libaio_queue *queue)
{
	struct lio *lio = queue->tio_data;
	uint64_t val;

	if (lio->flags & LIO_FLAG_EVENTFD) {
		int gcc = read(lio->event_fd, &val, sizeof(val));
		if (gcc) {};
	}
}

static void
libaio_backend_lio_event(event_id_t id, char mode, void *private)
{
	libaio_queue *queue = private;
	struct lio *lio;
	int i, ret, split;
	struct iocb *iocb;
	struct tiocb *tiocb;
	struct io_event *ep;

	libaio_backend_lio_ack_event(queue);

	lio   = queue->tio_data;
	/* io_getevents() invoked via the libaio wrapper does not set errno but
	 * instead returns -errno on error */
	while ((ret = user_io_getevents(lio->aio_ctx, queue->size, lio->aio_events)) < 0) {
		/* Permit some errors to retry */
		if (ret == -EINTR) continue;
		ERR(ret, "io_getevents() non-retryable error");
		return;
	}
	split = io_split(&queue->opioctx, lio->aio_events, ret);

	DBG("events: %d, tiocbs: %d\n", ret, split);

	queue->iocbs_pending  -= ret;
	queue->tiocbs_pending -= split;

	for (i = split, ep = lio->aio_events; i-- > 0; ep++) {
		iocb  = ep->obj;
		tiocb = iocb->data;
		complete_tiocb(queue, tiocb, ep->res);
	}

	queue_deferred_tiocbs(queue);
}

static int
libaio_backend_lio_setup(libaio_queue *queue, int qlen)
{
	struct lio *lio = queue->tio_data;
	int err;

	lio->event_id = -1;

	err = libaio_backend_lio_setup_aio(queue, qlen);
	if (err)
		goto fail;

	lio->event_id =
		tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					      lio->event_fd, TV_ZERO,
					      libaio_backend_lio_event,
					      queue);
	err = lio->event_id;
	if (err < 0)
		goto fail;

	lio->aio_events = calloc(qlen, sizeof(struct io_event));
	if (!lio->aio_events) {
		err = -errno;
		goto fail;
	}

	return 0;

fail:
	libaio_backend_lio_destroy(queue);
	return err;
}

static int
libaio_backend_lio_submit(libaio_queue *queue)
{
	struct lio *lio = queue->tio_data;
	int merged, submitted, err = 0;

	if (!queue->queued)
		return 0;

	merged    = io_merge(&queue->opioctx, queue->iocbs, queue->queued);
	libaio_backend_lio_set_eventfd(queue, merged, queue->iocbs);
	submitted = io_submit(lio->aio_ctx, merged, queue->iocbs);

	DBG("queued: %d, merged: %d, submitted: %d\n",
	    queue->queued, merged, submitted);

	if (submitted < 0) {
		err = submitted;
		submitted = 0;
	} else if (submitted < merged)
		err = -EIO;

	queue->iocbs_pending  += submitted;
	queue->tiocbs_pending += queue->queued;
	queue->queued          = 0;

	if (err)
		queue->tiocbs_pending -= 
			fail_tiocbs(queue, submitted, merged, err);

	return submitted;
}

static const struct tio td_tio_lio = {
	.name        = "lio",
	.data_size   = sizeof(struct lio),
	.tio_setup   = libaio_backend_lio_setup,
	.tio_destroy = libaio_backend_lio_destroy,
	.tio_submit  = libaio_backend_lio_submit,
};

static void
libaio_backend_queue_free_io(libaio_queue *queue)
{
	if (queue->tio) {
		if (queue->tio->tio_destroy)
			queue->tio->tio_destroy(queue);
		queue->tio = NULL;
	}

	if (queue->tio_data) {
		free(queue->tio_data);
		queue->tio_data = NULL;
	}
}

static int
libaio_backend_queue_init_io(libaio_queue *queue, int drv)
{
	const struct tio *tio;
	int err;

	switch (drv) {
	case TIO_DRV_LIO:
		tio = &td_tio_lio;
		break;
	default:
		err = -EINVAL;
		goto fail;
	}

	queue->tio_data = calloc(1, tio->data_size);
	if (!queue->tio_data) {
		PERROR("malloc(%zu)", tio->data_size);
		err = -errno;
		goto fail;
	}

	queue->tio = tio;

	if (tio->tio_setup) {
		err = tio->tio_setup(queue, queue->size);
		if (err)
			goto fail;
	}

	DPRINTF("I/O queue driver: %s\n", tio->name);

	return 0;

fail:
	libaio_backend_queue_free_io(queue);
	return err;
}

static void
libaio_backend_free_queue(tqueue *q)
{
	libaio_queue *queue = (libaio_queue*)*q;
	libaio_backend_queue_free_io(queue);

	free(queue->iocbs);
	queue->iocbs = NULL;
	opio_free(&queue->opioctx);
	free(queue);
	*q = NULL;
}




static int
libaio_backend_init_queue(tqueue *q, int size,
	int drv, struct tfilter *filter)
{

	int err;
	libaio_queue *queue = malloc(sizeof(libaio_queue));
	if(queue == NULL)
		return ENOMEM;

	*q = queue;

	memset(queue, 0, sizeof(libaio_queue));

	queue->size   = size;
	queue->filter = filter;

	if (!size)
		return 0;

	err = libaio_backend_queue_init_io(queue, drv);
	if (err)
		goto fail;

	queue->iocbs = calloc(size, sizeof(struct iocb *));
	if (!queue->iocbs) {
		err = -errno;
		goto fail;
	}

	err = opio_init(&queue->opioctx, size);
	if (err)
		goto fail;

	return 0;

 fail:
	libaio_backend_free_queue(q);
	return err;
}


static void 
libaio_backend_debug_queue(tqueue q)
{
	libaio_queue* queue = (libaio_queue*)q; 
	struct tiocb *tiocb = queue->deferred.head;

	WARN("LIBAIO QUEUE:\n");
	WARN("size: %d, tio: %s, queued: %d, iocbs_pending: %d, "
	     "tiocbs_pending: %d, tiocbs_deferred: %d, deferrals: %"PRIx64"\n",
	     queue->size, queue->tio->name, queue->queued, queue->iocbs_pending,
	     queue->tiocbs_pending, queue->tiocbs_deferred, queue->deferrals);

	if (tiocb) {
		WARN("deferred:\n");
		for (; tiocb != NULL; tiocb = tiocb->next) {
			struct iocb *io = (struct iocb*)tiocb->iocb;
			WARN("%s of %lu bytes at %lld\n",
			     iocb_opcode(io),
			     iocb_nbytes(io), iocb_offset(io));
		}
	}
}

static void
libaio_backend_prep_tiocb(struct tiocb *tiocb, int fd, int rw, char *buf, size_t size,
	long long offset, td_queue_callback_t cb, void *arg)
{
	tiocb->iocb = malloc(sizeof(struct iocb));
	struct iocb *iocb = (struct iocb*)tiocb->iocb;

	if (rw)
		io_prep_pwrite(iocb, fd, buf, size, offset);
	else
		io_prep_pread(iocb, fd, buf, size, offset);

	iocb->data  = tiocb;
	tiocb->cb   = cb;
	tiocb->arg  = arg;
	tiocb->next = NULL;
}

static void
libaio_backend_queue_tiocb(tqueue q, struct tiocb *tiocb)
{
	libaio_queue* queue = (libaio_queue*)q;

	if (!libaio_backend_queue_full(queue))
		queue_tiocb(queue, tiocb);
	else
		defer_tiocb(queue, tiocb);
}


/*
 * fail_tiocbs may queue more tiocbs
 */
static int
libaio_backend_submit_tiocbs(tqueue q)
{
	libaio_queue* queue = (libaio_queue*)q;
	return queue->tio->tio_submit(queue);
}

static int
libaio_backend_submit_all_tiocbs(tqueue q)
{
	int submitted = 0;

	libaio_queue* queue = (libaio_queue*)q;
	do {
		submitted += libaio_backend_submit_tiocbs(queue);
	} while (!libaio_backend_queue_empty(queue));

	return submitted;
}

/*
 * cancel_tiocbs may queue more tiocbs
 */
int
libaio_backend_cancel_tiocbs(tqueue q)
{
	libaio_queue* queue = (libaio_queue*)q;
	return cancel_tiocbs(queue, -EIO);
}

int
libaio_backend_cancel_all_tiocbs(tqueue q)
{
	int cancelled = 0;
	libaio_queue* queue = (libaio_queue*)q;

	do {
		cancelled += libaio_backend_cancel_tiocbs(queue);
	} while (!libaio_backend_queue_empty(queue));

	return cancelled;
}

struct backend* get_libaio_backend()
{
	static struct backend  lib_aio_backend = {
		.debug=libaio_backend_debug_queue,
		.init=libaio_backend_init_queue,
		.free_queue=libaio_backend_free_queue,
		.queue=libaio_backend_queue_tiocb,
		.submit_all=libaio_backend_submit_all_tiocbs,
		.submit_tiocbs=libaio_backend_submit_tiocbs,
		.prep=libaio_backend_prep_tiocb
	};
	return &lib_aio_backend;
}

