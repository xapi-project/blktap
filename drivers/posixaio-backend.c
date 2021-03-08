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
#define IO_SIGNAL SIGUSR1

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <aio.h>

#include "tapdisk.h"
#include "tapdisk-log.h"
#include "posixaio-backend.h"
#include "tapdisk-server.h"
#include "tapdisk-utils.h"
#include "timeout-math.h"
#include <signal.h>

#include "atomicio.h"
#include "aio_getevents.h"
#include <sys/signalfd.h>
#include "debug.h"


#define WARN(_f, _a...) tlog_write(TLOG_WARN, _f, ##_a)
#define DBG(_f, _a...) tlog_write(TLOG_DBG, _f, ##_a)
#define ERR(_err, _f, _a...) tlog_error(_err, _f, ##_a)

#define posixaio_backend_queue_empty(q) ((q)->queued == 0)
#define posixaio_backend_queue_full(q)  \
	(((q)->tiocbs_pending + (q)->queued) >= (q)->size)
typedef struct _posix_aio_queue {
	int                   size;

	const struct tio     *tio;
	void                 *tio_data;

	int                   queued;
	struct aiocb        **aiocbList;
	struct tiocb       **tiocbList;

	int                   tiocbs_pending;
	struct tlist          pending;

	struct tlist          deferred;
	int                   tiocbs_deferred;

	uint64_t              deferrals;
} posix_aio_queue;

struct tio {
	const char           *name;
	size_t                data_size;

	int  (*tio_setup)    (posix_aio_queue *queue, int qlen);
	void (*tio_destroy)  (posix_aio_queue *queue);
	int  (*tio_submit)   (posix_aio_queue *queue);
};

enum {
	TIO_DRV_LIO     = 1,
};

static inline void
queue_tiocb(posix_aio_queue *queue, struct tiocb *tiocb)
{
	queue->aiocbList[queue->queued] = &(tiocb->uiocb.aio);
	queue->tiocbList[queue->queued] = tiocb;
	queue->queued++;
}

static inline int
deferred_tiocbs(posix_aio_queue *queue)
{
	return (queue->deferred.head != NULL);
}

static inline void
defer_tiocb(posix_aio_queue *queue, struct tiocb *tiocb)
{
	struct tlist *list = &queue->deferred;

	if (!list->head)
		list->head = list->tail = tiocb;
	else
		list->tail = list->tail->next = tiocb;

	queue->tiocbs_deferred++;
	queue->deferrals++;
}

static void 
push_list(struct tlist *list, struct tiocb* tiocb)
{
	if (!list->head)
		list->head = list->tail = tiocb;
	else
		list->tail = list->tail->next = tiocb;
}

static void
pending_tiocb(posix_aio_queue *queue, struct tiocb *tiocb)
{
	struct tlist *list = &queue->pending;

	push_list(list, tiocb);
	queue->tiocbs_pending++;
}

static struct tiocb*
pop_pending_tiocb(posix_aio_queue *queue)
{
	struct tiocb *tiocb = NULL;
	struct tlist *list = &queue->pending;

	if (list->head == list->tail){
		tiocb = list->head;
		list->head = list->tail = NULL;
	} else {
		tiocb = list->head;
		list->head = tiocb->next;
	}

	queue->tiocbs_pending--;
	return tiocb;
}

static void 
shallow_copy_list(struct tlist *list, struct tlist *copy_to)
{
	copy_to->head = list->head;
	copy_to->tail = list->tail;
}

static void 
list_init(struct tlist *list)
{
	list->head = NULL;
	list->tail = NULL;
}

static inline void
queue_deferred_tiocb(posix_aio_queue *queue)
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
queue_deferred_tiocbs(posix_aio_queue *queue)
{
	while (!posixaio_backend_queue_full(queue) && deferred_tiocbs(queue))
		queue_deferred_tiocb(queue);
}

/*
 * td_complete may queue more tiocbs
 */
static void
complete_tiocb(posix_aio_queue *queue, struct tiocb *tiocb)
{
	int err;
	unsigned long actual_res;
	struct aiocb *aiocb = &(tiocb->uiocb.aio);
	unsigned long res = aiocb->aio_nbytes;

	/*TO DO THIS IS WRONG IN NORMAL QUEUE*/
	if (res == (actual_res = aio_return(aiocb)))
		err = 0;
	else if ((int)actual_res < 0)
		err = (int)res;
	else
		err = -EIO;

	tiocb->cb(tiocb->arg, tiocb, err);
}

struct lio {
	struct io_event *aio_events;

	int              event_fd;
	int              event_id;

	int              flags;
};

#define LIO_FLAG_EVENTFD        (1<<0)

static void
posixaio_backend_lio_destroy_aio(posix_aio_queue *queue)
{
	struct lio *lio = queue->tio_data;

	if (lio->event_fd >= 0) {
		close(lio->event_fd);
		lio->event_fd = -1;
	}
}

static int
__lio_setup_aio_eventfd(posix_aio_queue *queue, int qlen)
{
	struct lio *lio = queue->tio_data;
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, IO_SIGNAL);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
		return  -errno;

	lio->event_fd = signalfd(-1, &mask, 0);
	if (lio->event_fd == -1)
		return  -errno;

	return 0;
}

static int
posixaio_backend_lio_setup_aio(posix_aio_queue *queue, int qlen)
{
	struct lio *lio = queue->tio_data;
	int err = 0;

	lio->event_fd = -1;

	err = __lio_setup_aio_eventfd(queue, qlen);

	return err;
}

static void
posixaio_backend_lio_destroy(posix_aio_queue *queue)
{
	struct lio *lio = queue->tio_data;

	if (!lio)
		return;

	if (lio->event_id >= 0) {
		tapdisk_server_unregister_event(lio->event_id);
		lio->event_id = -1;
	}

	posixaio_backend_lio_destroy_aio(queue);

	if (lio->aio_events) {
		free(lio->aio_events);
		lio->aio_events = NULL;
	}
}

static void
posixaio_backend_lio_event(event_id_t id, char mode, void *private)
{
	posix_aio_queue *queue = private;
	struct tiocb *tiocb;
	struct tlist list;
	int tiocbs_pending = 0;
	
	list_init(&list);

	while((tiocb = pop_pending_tiocb(queue))) {
		if ( EINPROGRESS != aio_error(&(tiocb->uiocb.aio))) {
                	complete_tiocb(queue, tiocb);
		} else {
			push_list(&list, tiocb);
			tiocbs_pending++;
		}
	}

	shallow_copy_list(&list, &queue->pending);
	queue->tiocbs_pending = tiocbs_pending;

	queue_deferred_tiocbs(queue);
}

static int
posixaio_backend_lio_setup(posix_aio_queue *queue, int qlen)
{
	WARN("posixaio_backend_lio_setup");
	struct lio *lio = queue->tio_data;
	int err;

	lio->event_id = -1;

	err = posixaio_backend_lio_setup_aio(queue, qlen);
	if (err)
		goto fail;

	lio->event_id =
		tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					      lio->event_fd, TV_ZERO,
					      posixaio_backend_lio_event,
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
	posixaio_backend_lio_destroy(queue);
	return err;
}

static int
posixaio_backend_lio_submit(posix_aio_queue *queue)
{
	int j, err = 0, queued = queue->queued;
	struct aiocb **aiocbList = queue->aiocbList;
	struct tiocb **tiocbList = queue->tiocbList;
	if(queued == 0)
		return 0;
	
	for(j = 0; j < queue->queued; j++)
	{ 
		pending_tiocb(queue, tiocbList[j]);
	}

	ASSERT(queue->pending.tail == tiocbList[queued-1])
	err = lio_listio(LIO_NOWAIT, aiocbList, queued, NULL);

	if (err) {
		for(j = 0; j < queue->queued; j++)
		{ 
			aio_cancel(aiocbList[j]->aio_fildes, aiocbList[j]);
		}
	}

	queue->queued = 0;

	return queued;
}

static const struct tio td_tio_lio = {
	.name        = "lio",
	.data_size   = sizeof(struct lio),
	.tio_setup   = posixaio_backend_lio_setup,
	.tio_destroy = posixaio_backend_lio_destroy,
	.tio_submit  = posixaio_backend_lio_submit,
};

static void
posixaio_backend_queue_free_io(posix_aio_queue *queue)
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
posixaio_backend_queue_init_io(posix_aio_queue *queue, int drv)
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

	queue->tiocbs_pending = 0;
	list_init(&queue->pending);

	queue->tiocbs_deferred = 0;
	list_init(&queue->deferred);


	DPRINTF("I/O queue driver: %s\n", tio->name);

	return 0;

fail:
	posixaio_backend_queue_free_io(queue);
	return err;
}

static void
posixaio_backend_free_queue(tqueue* pqueue)
{
	posix_aio_queue* queue = (posix_aio_queue*)*pqueue;
	posixaio_backend_queue_free_io(queue);

	free(queue->aiocbList);
	queue->aiocbList = NULL;
	free(queue->tiocbList);
	queue->tiocbList = NULL;
	free(queue);
	*pqueue = NULL;
}

static int
posixaio_backend_init_queue(tqueue *pqueue, int size,
		   int drv, struct tfilter *filter)
{
	int err;
	*pqueue = (tqueue)malloc(sizeof(posix_aio_queue));
	posix_aio_queue *queue = *pqueue;
	if(queue == NULL)
		return ENOMEM;

	memset(queue, 0, sizeof(posix_aio_queue));

	queue->size   = size;

	if (!size)
		return 0;

	err = posixaio_backend_queue_init_io(queue, drv);
	if (err){
		WARN("error from posixaio_backend_queue_init_io\n");
		goto fail;
	}

	queue->aiocbList = calloc(size, sizeof(struct aiocb*));
	if (!queue->aiocbList) {
		WARN("could not alloc aiocblist\n");
		err = -errno;
		goto fail;
	}
	queue->tiocbList = calloc(size, sizeof(struct tiocb*));
	if (!queue->tiocbList) {
		WARN("could not alloc tiocblist\n");
		err = -errno;
		goto fail;
	}

	return 0;

 fail:
	posixaio_backend_free_queue(pqueue);
	return err;
}

static void 
posixaio_backend_debug_queue(tqueue q)
{
	posix_aio_queue* queue = (posix_aio_queue*)q;
	struct tiocb *tiocb = queue->deferred.head;

	WARN("POSIX AIO QUEUE:\n");
	WARN("size: %d, queued: %d, "
	     "tiocbs_pending: %d, tiocbs_deferred: %d, deferrals: %"PRIx64"\n",
	     queue->size, queue->queued,
	     queue->tiocbs_pending, queue->tiocbs_deferred, queue->deferrals);

	if (tiocb) {
		WARN("deferred:\n");
		for (; tiocb != NULL; tiocb = tiocb->next) {
			struct aiocb *aiocb = &(tiocb->uiocb.aio);
			char* op = aiocb->aio_lio_opcode == LIO_WRITE ? "read" : "write";
			WARN("%s of %lu bytes at %jd\n",
			     op, aiocb->aio_nbytes,			     
			     aiocb->aio_offset);
		}
	}
}

void
posixaio_backend_prep_tiocb(struct tiocb *tiocb, int fd, int rw, char *buf, size_t size,
		   long long offset, td_queue_callback_t cb, void *arg)
{
	struct aiocb *aiocb = &(tiocb->uiocb.aio);

	aiocb->aio_fildes = fd;
	aiocb->aio_buf = buf; 
	aiocb->aio_nbytes = size;
	aiocb->aio_reqprio = 0;
	aiocb->aio_offset = offset;
	aiocb->aio_sigevent.sigev_notify = SIGEV_SIGNAL;
	aiocb->aio_sigevent.sigev_signo = IO_SIGNAL;
	aiocb->aio_sigevent.sigev_value.sival_ptr = NULL;

	aiocb->aio_lio_opcode = rw ? LIO_WRITE : LIO_READ;

	tiocb->cb   = cb;
	tiocb->arg  = arg;
	tiocb->next = NULL;
}

static void
posixaio_backend_queue_tiocb(tqueue q, struct tiocb *tiocb)
{
	posix_aio_queue* queue = (posix_aio_queue*)q;
	if (!posixaio_backend_queue_full(queue))
		queue_tiocb(queue, tiocb);
	else
		defer_tiocb(queue, tiocb);
}

static int
posixaio_backend_submit_tiocbs(tqueue q)
{
	posix_aio_queue* queue = (posix_aio_queue*)q;
	return queue->tio->tio_submit(queue);
}

static int
posixaio_backend_submit_all_tiocbs(tqueue q)
{
	posix_aio_queue* queue = (posix_aio_queue*)q;
	int submitted = 0;

	do {
		submitted += posixaio_backend_submit_tiocbs(queue);
	} while (!posixaio_backend_queue_empty(queue));

	return submitted;
}

struct backend* get_posix_aio_backend()
{
	static struct backend  posix_aio_backend = {
		.debug=posixaio_backend_debug_queue,
		.init=posixaio_backend_init_queue,
		.free_queue=posixaio_backend_free_queue,
		.queue=posixaio_backend_queue_tiocb,
		.submit_all=posixaio_backend_submit_all_tiocbs,
		.submit_tiocbs=posixaio_backend_submit_tiocbs,
		.prep=posixaio_backend_prep_tiocb
	};
	return &posix_aio_backend;
}
