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
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/resource.h>

#define TAPDISK
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"

#define DBG(_level, _f, _a...)       tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...)         tlog_error(_err, _f, ##_a)

#define TAPDISK_TIOCBS              (TAPDISK_DATA_REQUESTS + 50)

typedef struct tapdisk_server {
	int                          run;
	td_ipc_t                     ipc;
	struct list_head             vbds;
	scheduler_t                  scheduler;
	event_id_t                   control_event;
	struct tqueue                aio_queue;
	event_id_t                   aio_queue_event_id;
} tapdisk_server_t;

static tapdisk_server_t server;

#define tapdisk_server_for_each_vbd(vbd, tmp)			        \
	list_for_each_entry_safe(vbd, tmp, &server.vbds, next)

struct tap_disk *
tapdisk_server_find_driver_interface(int type)
{
	int n;

	n = sizeof(dtypes) / sizeof(struct disk_info_t *);
	if (type > n)
		return NULL;

	return dtypes[type]->drv;
}

td_image_t *
tapdisk_server_get_shared_image(td_image_t *image)
{
	td_vbd_t *vbd, *tmpv;
	td_image_t *img, *tmpi;

	if (!td_flag_test(image->flags, TD_OPEN_SHAREABLE))
		return NULL;

	tapdisk_server_for_each_vbd(vbd, tmpv)
		tapdisk_vbd_for_each_image(vbd, img, tmpi)
			if (img->type == image->type &&
			    !strcmp(img->name, image->name))
				return img;

	return NULL;
}

td_vbd_t *
tapdisk_server_get_vbd(uint16_t uuid)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		if (vbd->uuid == uuid)
			return vbd;

	return NULL;
}

void
tapdisk_server_add_vbd(td_vbd_t *vbd)
{
	list_add_tail(&vbd->next, &server.vbds);
}

void
tapdisk_server_remove_vbd(td_vbd_t *vbd)
{
	list_del(&vbd->next);
	INIT_LIST_HEAD(&vbd->next);
	tapdisk_server_check_state();
}

void
tapdisk_server_queue_tiocb(struct tiocb *tiocb)
{
	tapdisk_queue_tiocb(&server.aio_queue, tiocb);
}

void
tapdisk_server_debug(void)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_debug_queue(&server.aio_queue);

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_debug(vbd);

	tlog_flush();
}

void
tapdisk_server_check_state(void)
{
	if (list_empty(&server.vbds))
		server.run = 0;
}

event_id_t
tapdisk_server_register_event(char mode, int fd,
			      int timeout, event_cb_t cb, void *data)
{
	return scheduler_register_event(&server.scheduler,
					mode, fd, timeout, cb, data);
}

void
tapdisk_server_unregister_event(event_id_t event)
{
	return scheduler_unregister_event(&server.scheduler, event);
}

void
tapdisk_server_set_max_timeout(int seconds)
{
	scheduler_set_max_timeout(&server.scheduler, seconds);
}

static void
tapdisk_server_assert_locks(void)
{

}

static void
tapdisk_server_set_retry_timeout(void)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		if (tapdisk_vbd_retry_needed(vbd)) {
			tapdisk_server_set_max_timeout(TD_VBD_RETRY_INTERVAL);
			return;
		}
}

static void
tapdisk_server_check_progress(void)
{
	struct timeval now;
	td_vbd_t *vbd, *tmp;

	gettimeofday(&now, NULL);

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_check_progress(vbd);
}

static void
tapdisk_server_submit_tiocbs(void)
{
	tapdisk_submit_all_tiocbs(&server.aio_queue);
}

static void
tapdisk_server_kick_responses(void)
{
	int n;
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_kick(vbd);
}

static void
tapdisk_server_check_vbds(void)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_check_state(vbd);
}

static void
tapdisk_server_read_ipc_message(event_id_t id, char mode, void *private)
{
	tapdisk_ipc_read(&server.ipc);
}

static void
tapdisk_server_aio_queue_event(event_id_t id, char mode, void *private)
{
	tapdisk_complete_tiocbs(&server.aio_queue);
}

static void
tapdisk_server_free_aio_queue(void)
{
	tapdisk_server_unregister_event(server.aio_queue_event_id);
	tapdisk_free_queue(&server.aio_queue);
}

static int
tapdisk_server_initialize_aio_queue(void)
{
	int err;
	event_id_t id;

	err = tapdisk_init_queue(&server.aio_queue,
				 TAPDISK_TIOCBS, 0, NULL);
	if (err)
		return err;

	id = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
					   server.aio_queue.poll_fd, 0,
					   tapdisk_server_aio_queue_event,
					   NULL);
	if (id < 0) {
		tapdisk_free_queue(&server.aio_queue);
		return id;
	}

	server.aio_queue_event_id = id;

	return 0;
}

static int
tapdisk_server_initialize(char *read, char *write)
{
	int err;
	event_id_t event_id;

	event_id = 0;
	memset(&server, 0, sizeof(tapdisk_server_t));

	INIT_LIST_HEAD(&server.vbds);

	server.ipc.rfd = open(read, O_RDWR | O_NONBLOCK);
	server.ipc.wfd = open(write, O_RDWR | O_NONBLOCK);
	if (server.ipc.rfd < 0 || server.ipc.wfd < 0) {
		EPRINTF("FD open failed [%d, %d]\n",
			server.ipc.rfd, server.ipc.wfd);
		err = (errno ? -errno : -EIO);
		goto fail;
	}

	scheduler_initialize(&server.scheduler);

	event_id = scheduler_register_event(&server.scheduler,
					    SCHEDULER_POLL_READ_FD,
					    server.ipc.rfd, 0,
					    tapdisk_server_read_ipc_message,
					    NULL);
	if (event_id < 0) {
		err = event_id;
		goto fail;
	}

	err = tapdisk_server_initialize_aio_queue();
	if (err)
		goto fail;

	server.control_event = event_id;
	server.run = 1;

	return 0;

fail:
	if (server.ipc.rfd > 0)
		close(server.ipc.rfd);
	if (server.ipc.wfd > 0)
		close(server.ipc.wfd);
	if (event_id > 0)
		scheduler_unregister_event(&server.scheduler,
					   server.control_event);
	return err;
}

static void
tapdisk_server_close(void)
{
	tapdisk_server_free_aio_queue();
	scheduler_unregister_event(&server.scheduler, server.control_event);
	close(server.ipc.rfd);
	close(server.ipc.wfd);
}

static void
tapdisk_server_run(void)
{
	int ret;

	while (server.run) {
		tapdisk_server_assert_locks();
		tapdisk_server_set_retry_timeout();
		tapdisk_server_check_progress();

		ret = scheduler_wait_for_events(&server.scheduler);
		if (ret < 0) {
			DBG(TLOG_WARN, "server wait returned %d\n", ret);
			sleep(2);
		}

		tapdisk_server_check_vbds();
		tapdisk_server_submit_tiocbs();
		tapdisk_server_kick_responses();
	}
}

static void
tapdisk_server_signal_handler(int signal)
{
	switch (signal) {
	case SIGBUS:
	case SIGINT:
		tapdisk_server_check_state();
		break;

	case SIGUSR1:
		tapdisk_server_debug();
		break;
	}
}

static void
usage(void)
{
	fprintf(stderr, "blktap-utils: v2.0.0\n");
	fprintf(stderr, "usage: tapdisk <READ fifo> <WRITE fifo>\n");
        exit(-EINVAL);
}

int
main(int argc, char *argv[])
{
	int err;
	char buf[128];
	struct rlimit rlim;

	if (argc != 3)
		usage();

	daemon(0, 0);

	snprintf(buf, sizeof(buf), "TAPDISK[%d]", getpid());
	openlog(buf, LOG_CONS | LOG_ODELAY, LOG_DAEMON);
	open_tlog("/tmp/tapdisk.log", (64 << 10), TLOG_WARN, 0);

	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;

	err = setrlimit(RLIMIT_MEMLOCK, &rlim);
	if (err == -1) {
		EPRINTF("RLIMIT_MEMLOCK failed: %d\n", errno);
		return -errno;
	}

	err = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (err == -1) {
		EPRINTF("mlockall failed: %d\n", errno);
		return -errno;
	}

#if defined(CORE_DUMP)
	err = setrlimit(RLIMIT_CORE, &rlim);
	if (err == -1)
		EPRINTF("RLIMIT_CORE failed: %d\n", errno);
#endif

	err = tapdisk_server_initialize(argv[1], argv[2]);
	if (err) {
		EPRINTF("failed to allocate tapdisk server: %d\n", err);
		exit(err);
	}

	signal(SIGBUS, tapdisk_server_signal_handler);
	signal(SIGINT, tapdisk_server_signal_handler);
	signal(SIGUSR1, tapdisk_server_signal_handler);

	tapdisk_server_run();

	tapdisk_server_close();

	closelog();
	close_tlog();

	return 0;
}
