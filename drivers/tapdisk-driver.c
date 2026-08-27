/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>

#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-disktype.h"
#include "tapdisk-stats.h"

static void
tapdisk_driver_log_flush(td_driver_t *driver, const char *__caller)
{
	td_loglimit_t *rl = &driver->loglimit;

	if (rl->dropped) {
		tlog_syslog(TLOG_WARN,
			    "%s: %s: %d messages suppressed",
			    driver->name, __caller, rl->dropped);
		rl->dropped = 0;
	}
}

int
tapdisk_driver_log_pass(td_driver_t *driver, const char *__caller)
{
	td_loglimit_t *rl = &driver->loglimit;
	int dropping = rl->dropped;

	if (tapdisk_loglimit_pass(rl)) {
		tapdisk_driver_log_flush(driver, __caller);
		return 1;
	}

	if (!dropping)
		tlog_syslog(TLOG_WARN,
			    "%s: %s: too many errors, dropped.",
			    driver->name, __caller);

	return 0;
}

td_driver_t *
tapdisk_driver_allocate(int type, const char *name, td_flag_t flags)
{
	int err;
	td_driver_t *driver;
	const struct tap_disk *ops;

	ops = tapdisk_disk_drivers[type];
	if (!ops)
		return NULL;

	driver = calloc(1, sizeof(td_driver_t));
	if (!driver)
		return NULL;

	err = tapdisk_namedup(&driver->name, name);
	if (err)
		goto fail;

	driver->ops     = ops;
	driver->type    = type;
	driver->storage = -1;
	driver->data    = calloc(1, ops->private_data_size);
	if (!driver->data)
		goto fail;

	if (td_flag_test(flags, TD_OPEN_RDONLY)){
		td_flag_set(driver->state, TD_DRIVER_RDONLY);
		driver->prep_func = tapdisk_server_prep_tiocb_ro;
		driver->queue_func = tapdisk_server_queue_tiocb_ro;
	} else {
		driver->prep_func = tapdisk_server_prep_tiocb;
		driver->queue_func = tapdisk_server_queue_tiocb;
	}

	tapdisk_loglimit_init(&driver->loglimit,
			      16 /* msgs */,
			      90 * 1000 /* ms */);

	return driver;

fail:
	free(driver->name);
	free(driver->data);
	free(driver);
	return NULL;
}

void
tapdisk_driver_free(td_driver_t *driver)
{
	if (!driver)
		return;

	if (driver->refcnt)
		return;

	if (td_flag_test(driver->state, TD_DRIVER_OPEN))
		EPRINTF("freeing open driver %s (state 0x%08x)\n",
			driver->name, driver->state);

	tapdisk_driver_log_flush(driver, __func__);

	free(driver->name);
	free(driver->data);
	free(driver);
}

void
tapdisk_driver_prep_tiocb(td_driver_t *driver, struct tiocb *tiocb, int fd, int rw, char *buf, size_t size,
		   long long offset, td_queue_callback_t cb, void *arg)
{
	driver->prep_func(tiocb, fd, rw, buf, size, offset, cb, arg);
}

void
tapdisk_driver_queue_tiocb(td_driver_t *driver, struct tiocb *tiocb)
{
	driver->queue_func(tiocb);
}

void
tapdisk_driver_debug(td_driver_t *driver)
{
	if (driver->ops->td_debug)
		driver->ops->td_debug(driver);
}

void
tapdisk_driver_stats(td_driver_t *driver, td_stats_t *st)
{
	const disk_info_t *info;

	tapdisk_stats_field(st, "type", "d", driver->type);

	info = tapdisk_disk_types[driver->type];
	tapdisk_stats_field(st, "name", "s", info->name);

	if (driver->ops->td_stats) {
		tapdisk_stats_field(st, "status", "{");
		driver->ops->td_stats(driver, st);
		tapdisk_stats_leave(st, '}');
	} else
		tapdisk_stats_field(st, "status", NULL);

}
