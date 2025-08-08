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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#ifdef HAVE_EVENTFD
#include <sys/eventfd.h>
#else
#include <sys/syscall.h>
#endif
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include "tapdisk-syslog.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "posixaio-backend.h"
#include "libaio-backend.h"
#include "tapdisk-interface.h"
#include "tapdisk-log.h"
#include "td-blkif.h"
#include "timeout-math.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include "../cpumond/cpumond.h"

#define DBG(_level, _f, _a...)       tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...)         tlog_error(_err, _f, ##_a)

#define TAPDISK_TIOCBS              (TAPDISK_DATA_REQUESTS + 50)

typedef struct tapdisk_server {
	int                          run;
	struct list_head             vbds;
	scheduler_t                  scheduler;
	tqueue                       rw_queue;
	tqueue                       ro_queue;
	struct backend              *ro_backend;
	struct backend              *rw_backend;
	char                        *name;
	char                        *ident;
	int                          facility;

	/* CPU Utilisation Monitor client state */
	struct {
		int                         fd; /* shm fd */
		cpumond_t                  *cpumon; /* mmap pointer */
	} cpumond_state;

	event_id_t                   tlog_reopen_evid;
} tapdisk_server_t;

static tapdisk_server_t server;

unsigned int PAGE_SIZE;
unsigned int PAGE_MASK;
unsigned int PAGE_SHIFT;

#define tapdisk_server_for_each_vbd(vbd, tmp)			        \
	list_for_each_entry_safe(vbd, tmp, &server.vbds, next)

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

struct list_head *
tapdisk_server_get_all_vbds(void)
{
	return &server.vbds;
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
tapdisk_server_prep_tiocb(struct tiocb *tiocb, int fd, int rw, char *buf, size_t size,
	long long offset, td_queue_callback_t cb, void *arg)
{
	server.rw_backend->prep(tiocb, fd, rw, buf, size, offset, cb, arg);
}

void
tapdisk_server_queue_tiocb(struct tiocb *tiocb)
{
	server.rw_backend->queue(server.rw_queue, tiocb);
}

void
tapdisk_server_prep_tiocb_ro(struct tiocb *tiocb, int fd, int rw, char *buf, size_t size,
	long long offset, td_queue_callback_t cb, void *arg)
{
	server.ro_backend->prep(tiocb, fd, rw, buf, size, offset, cb, arg);
}

void
tapdisk_server_queue_tiocb_ro(struct tiocb *tiocb)
{
	server.ro_backend->queue(server.ro_queue, tiocb);
}

void
tapdisk_server_debug(void)
{
	td_vbd_t *vbd, *tmp;

	if (likely(server.rw_queue))
		server.rw_backend->debug(server.rw_queue);
	if (likely(server.ro_queue))
		server.ro_backend->debug(server.ro_queue);

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_debug(vbd);

	DBG(TLOG_INFO, "debug log completed\n");
	tlog_precious(1);
}

void
tapdisk_server_check_state(void)
{
	if (list_empty(&server.vbds))
		server.run = 0;
}

event_id_t
tapdisk_server_register_event(char mode, int fd,
			      struct timeval timeout, event_cb_t cb, void *data)
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
tapdisk_server_mask_event(event_id_t event, int masked)
{
	return scheduler_mask_event(&server.scheduler, event, masked);
}

void
tapdisk_server_set_max_timeout(int seconds)
{
	scheduler_set_max_timeout(&server.scheduler, TV_SECS(seconds));
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
	server.rw_backend->submit_all(server.rw_queue);
	server.ro_backend->submit_all(server.ro_queue);
}

static void
tapdisk_server_kick_responses(void)
{
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

/**
 * Issues new requests. Returns the number of VBDs that contained new requests
 * which have been issued.
 */
static int
tapdisk_server_recheck_vbds(void)
{
	td_vbd_t *vbd, *tmp;
	int rv = 0;

	tapdisk_server_for_each_vbd(vbd, tmp)
		rv += tapdisk_vbd_recheck_state(vbd);

	return rv;
}

static void
tapdisk_server_stop_vbds(void)
{
	td_vbd_t *vbd, *tmp;

	tapdisk_server_for_each_vbd(vbd, tmp)
		tapdisk_vbd_kill_queue(vbd);
}

static int
tapdisk_server_init_aio(void)
{
	int err;
       	err = server.ro_backend->init(&server.ro_queue, TAPDISK_TIOCBS,
				  TIO_DRV_LIO, NULL);
	if(err)
		return err;
	
	return server.rw_backend->init(&server.rw_queue, TAPDISK_TIOCBS,
				  TIO_DRV_LIO, NULL);
}

static void
tapdisk_server_close_aio(void)
{
	server.rw_backend->free_queue(&server.rw_queue);
	server.ro_backend->free_queue(&server.ro_queue);
}

int
tapdisk_server_openlog(const char *name, int options, int facility)
{
	server.facility = facility;
	server.name     = strdup(name);
	server.ident    = tapdisk_syslog_ident(name);

	if (!server.name || !server.ident)
		return -errno;

	openlog(server.ident, options, facility);

	return 0;
}

void
tapdisk_server_closelog(void)
{
	closelog();

	free(server.name);
	server.name = NULL;

	free(server.ident);
	server.ident = NULL;
}

static int
tapdisk_server_open_tlog(void)
{
	int err = 0;

	if (server.name)
		err = tlog_open(server.name, server.facility, TLOG_WARN);

	return err;
}

static void
tapdisk_server_close_tlog(void)
{
	tlog_close();
}

static void
tapdisk_server_close(void)
{
	if (likely(server.tlog_reopen_evid >= 0))
		tapdisk_server_unregister_event(server.tlog_reopen_evid);

	tapdisk_server_close_tlog();
	tapdisk_server_close_aio();
}

void
tapdisk_server_iterate(void)
{
	int ret;

	tapdisk_server_assert_locks();
	tapdisk_server_set_retry_timeout();
	tapdisk_server_check_progress();

	ret = scheduler_wait_for_events(&server.scheduler);
	if (ret < 0)
		DBG(TLOG_WARN, "server wait returned %s\n", strerror(-ret));

	tapdisk_server_check_vbds();
	do {
		tapdisk_server_submit_tiocbs();
		tapdisk_server_kick_responses();

		ret = tapdisk_server_recheck_vbds();
	} while (ret); /* repeat until there are no new requests to issue */
}

static void
__tapdisk_server_run(void)
{
	while (server.run)
		tapdisk_server_iterate();
}

static void
tapdisk_server_signal_handler(int signal)
{
	td_vbd_t *vbd, *tmp;
	struct td_xenblkif *blkif;
	static int xfsz_error_sent = 0;

	switch (signal) {
	case SIGBUS:
	case SIGINT:
		tapdisk_server_for_each_vbd(vbd, tmp)
			tapdisk_vbd_close(vbd);
		break;

	case SIGXFSZ:
		ERR(EFBIG, "received SIGXFSZ");
		tapdisk_server_stop_vbds();
		if (xfsz_error_sent)
			break;

		xfsz_error_sent = 1;
		break;

	case SIGUSR1:
		DBG(TLOG_INFO, "debugging on signal %d\n", signal);
		tapdisk_server_debug();
		break;

	case SIGUSR2:
		DBG(TLOG_INFO, "triggering polling on signal %d\n", signal);
		tapdisk_server_for_each_vbd(vbd, tmp)
			list_for_each_entry(blkif, &vbd->rings, entry)
				tapdisk_start_polling(blkif);
		break;

	case SIGHUP:
		tapdisk_server_event_set_timeout(server.tlog_reopen_evid, TV_ZERO);
		break;
	}
}


static void
tlog_reopen_cb(event_id_t id, char mode __attribute__((unused)), void *private)
{
	tlog_reopen();
	tapdisk_server_event_set_timeout(id, TV_INF);
}

static void cpumond_state_init(void)
{
	server.cpumond_state.fd = -1;
	server.cpumond_state.cpumon = (cpumond_t *) 0;
}

static void cpumond_cleanup(void)
{
	if (server.cpumond_state.cpumon)
		munmap(server.cpumond_state.cpumon, sizeof(cpumond_t));
	if (server.cpumond_state.fd >= 0)
		close(server.cpumond_state.fd);

	cpumond_state_init();
}

float
tapdisk_server_system_idle_cpu(void)
{
	if (server.cpumond_state.cpumon > 0)
		return server.cpumond_state.cpumon->idle;
	else
		return 0.0;
}

/* Create the CPU Utilisation Monitor client. */
static int
tapdisk_server_initialize_cpumond_client(void)
{
	server.cpumond_state.fd = shm_open(CPUMOND_PATH, O_RDONLY, 0);
	if (server.cpumond_state.fd == -1)
		return -errno;

	server.cpumond_state.cpumon = mmap(NULL, sizeof(cpumond_t), PROT_READ, MAP_PRIVATE, server.cpumond_state.fd, 0);
	if (server.cpumond_state.cpumon == (cpumond_t *) -1) {
		server.cpumond_state.cpumon = 0;
		return -errno;
	}

	return 0;
}

int
tapdisk_server_init(void)
{
	int ret;
	unsigned int i = 0;

	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	PAGE_MASK = ~(PAGE_SIZE - 1);

	for (i = PAGE_SIZE, PAGE_SHIFT = 0; i > 1; i >>= 1, PAGE_SHIFT++);

	memset(&server, 0, sizeof(server));
	INIT_LIST_HEAD(&server.vbds);

	scheduler_initialize(&server.scheduler);

	if ((ret = tapdisk_server_initialize_cpumond_client()) < 0) {
		EPRINTF("Failed to connect to cpumond: %s\n",
			strerror(-ret));
		cpumond_cleanup();
	}

	server.tlog_reopen_evid = -1;

	return 0;
}

int
tapdisk_server_complete(void)
{
	int err;
	server.rw_backend = get_libaio_backend();
	server.ro_backend = get_libaio_backend();

	server.rw_backend = get_libaio_backend();
	err = tapdisk_server_init_aio();
	if (err)
		goto fail;

	err = tapdisk_server_open_tlog();
	if (err)
		goto fail;

	server.run = 1;

	return 0;

fail:
	tapdisk_server_close_tlog();
	tapdisk_server_close_aio();
	return err;
}

int
tapdisk_server_initialize(const char *read, const char *write)
{
	int err;

	tapdisk_server_init();

	err = tapdisk_server_complete();
	if (err)
		goto fail;

	return 0;

fail:
	tapdisk_server_close();
	return err;
}

int
tapdisk_server_run()
{
	int err;

	err = tapdisk_set_resource_limits();
	if (err)
		return err;

	signal(SIGBUS, tapdisk_server_signal_handler);
	signal(SIGINT, tapdisk_server_signal_handler);
	signal(SIGUSR1, tapdisk_server_signal_handler);
	signal(SIGUSR2, tapdisk_server_signal_handler);
	signal(SIGHUP, tapdisk_server_signal_handler);
	signal(SIGXFSZ, tapdisk_server_signal_handler);

	err = tapdisk_server_register_event(SCHEDULER_POLL_TIMEOUT, -1,	TV_INF,
			tlog_reopen_cb,	NULL);
	if (unlikely(err < 0)) {
		EPRINTF("failed to register reopen log event: %s\n", strerror(-err));
		goto out;
	}

	server.tlog_reopen_evid = err;

	err = 0;

	__tapdisk_server_run();

out:
	tapdisk_server_close();

	return err;
}

int
tapdisk_server_event_set_timeout(event_id_t event_id, struct timeval timeo) {
	return scheduler_event_set_timeout(&server.scheduler, event_id, timeo);
}

