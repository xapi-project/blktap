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
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/signal.h>

#include "tapdisk-syslog.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-log.h"

#define DBG(_level, _f, _a...)       tlog_write(_level, _f, ##_a)
#define ERR(_err, _f, _a...)         tlog_error(_err, _f, ##_a)

#define TAPDISK_TIOCBS              (TAPDISK_DATA_REQUESTS + 50)

typedef struct tapdisk_server {
	int                          run;
	struct list_head             vbds;
	scheduler_t                  scheduler;
	struct tqueue                aio_queue;
	char                        *name;
	char                        *ident;
	int                          facility;
} tapdisk_server_t;

static tapdisk_server_t server;

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
tapdisk_server_mask_event(event_id_t event, int masked)
{
	return scheduler_mask_event(&server.scheduler, event, masked);
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
	return tapdisk_init_queue(&server.aio_queue, TAPDISK_TIOCBS,
				  TIO_DRV_LIO, NULL);
}

static void
tapdisk_server_close_aio(void)
{
	tapdisk_free_queue(&server.aio_queue);
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
	} while (ret);
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

	case SIGHUP:
		tlog_reopen();
		break;
	}
}

int
tapdisk_server_init(void)
{
	memset(&server, 0, sizeof(server));
	INIT_LIST_HEAD(&server.vbds);

	scheduler_initialize(&server.scheduler);

	return 0;
}

int
tapdisk_server_complete(void)
{
	int err;

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
	signal(SIGHUP, tapdisk_server_signal_handler);
	signal(SIGXFSZ, tapdisk_server_signal_handler);

	__tapdisk_server_run();
	tapdisk_server_close();

	return 0;
}

int
tapdisk_server_event_set_timeout(event_id_t event_id, int timeo) {
	return scheduler_event_set_timeout(&server.scheduler, event_id, timeo);
}

