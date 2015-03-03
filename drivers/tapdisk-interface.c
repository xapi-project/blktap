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

#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include "tapdisk.h"
#include "tapdisk-vbd.h"
#include "tapdisk-image.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-interface.h"
#include "tapdisk-log.h"

int
td_load(td_image_t *image)
{
	td_image_t *shared;
	td_driver_t *driver;

	shared = tapdisk_server_get_shared_image(image);
	if (!shared)
		return -ENODEV;

	driver = shared->driver;
	if (!driver)
		return -EBADF;

	driver->refcnt++;
	image->driver = driver;
	image->info   = driver->info;

	DPRINTF("loaded shared image %s (%d users, state: 0x%08x, type: %d)\n",
		driver->name, driver->refcnt, driver->state, driver->type);
	return 0;
}

int
__td_open(td_image_t *image, td_disk_info_t *info)
{
	int err;
	td_driver_t *driver;

	driver = image->driver;
	if (!driver) {
		driver = tapdisk_driver_allocate(image->type,
						 image->name,
						 image->flags);
		if (!driver)
			return -ENOMEM;

		if (info) /* pre-seed driver->info for virtual drivers */
			driver->info = *info;
	}

	if (!td_flag_test(driver->state, TD_DRIVER_OPEN)) {
		err = driver->ops->td_open(driver, image->name, image->flags);
		if (err) {
			if (!image->driver)
				tapdisk_driver_free(driver);
			return err;
		}

		td_flag_set(driver->state, TD_DRIVER_OPEN);
		DPRINTF("opened image %s (%d users, state: 0x%08x, type: %d, %s)\n",
			driver->name, driver->refcnt + 1,
			driver->state, driver->type,
			td_flag_test(image->flags, TD_OPEN_RDONLY) ? "ro" : "rw");
	}

	image->driver = driver;
	image->info   = driver->info;
	driver->refcnt++;
	return 0;
}

int
td_open(td_image_t *image)
{
	return __td_open(image, NULL);
}

int
td_close(td_image_t *image)
{
	td_driver_t *driver;

	driver = image->driver;
	if (!driver)
		return -ENODEV;

	driver->refcnt--;
	if (!driver->refcnt && td_flag_test(driver->state, TD_DRIVER_OPEN)) {
		driver->ops->td_close(driver);
		td_flag_clear(driver->state, TD_DRIVER_OPEN);
	}

	DPRINTF("closed image %s (%d users, state: 0x%08x, type: %d)\n",
		driver->name, driver->refcnt, driver->state, driver->type);

	return 0;
}

int
td_get_oflags(td_image_t *image, int *flags)
{
	td_driver_t *driver;

	driver = image->driver;
	if (!driver)
		return -ENODEV;

	if (!td_flag_test(driver->state, TD_DRIVER_OPEN))
		return -EBADF;

	if (!driver->ops->td_get_oflags)
		return -ENOTSUP;

	return driver->ops->td_get_oflags(driver, flags);
}

int
td_get_parent_id(td_image_t *image, td_disk_id_t *id)
{
	td_driver_t *driver;

	driver = image->driver;
	if (!driver)
		return -ENODEV;

	if (!td_flag_test(driver->state, TD_DRIVER_OPEN))
		return -EBADF;

	return driver->ops->td_get_parent_id(driver, id);
}

int
td_validate_parent(td_image_t *image, td_image_t *parent)
{
	td_driver_t *driver, *pdriver;

	driver  = image->driver;
	pdriver = parent->driver;
	if (!driver || !pdriver)
		return -ENODEV;

	if (!td_flag_test(driver->state, TD_DRIVER_OPEN) ||
	    !td_flag_test(pdriver->state, TD_DRIVER_OPEN))
		return -EBADF;

	return 0;
	return driver->ops->td_validate_parent(driver, pdriver, 0);
}

void
td_queue_write(td_image_t *image, td_request_t treq)
{
	int err;
	td_driver_t *driver;

	driver = image->driver;
	if (!driver) {
		err = -ENODEV;
		goto fail;
	}

	if (!td_flag_test(driver->state, TD_DRIVER_OPEN)) {
		err = -EBADF;
		goto fail;
	}

	if (!driver->ops->td_queue_write) {
		err = -EOPNOTSUPP;
		goto fail;
	}

	err = tapdisk_image_check_td_request(image, treq);
	if (err)
		goto fail;

	driver->ops->td_queue_write(driver, treq);

	return;

fail:
	td_complete_request(treq, err);
}

void
td_queue_read(td_image_t *image, td_request_t treq)
{
	int err;
	td_driver_t *driver;

	driver = image->driver;
	if (!driver) {
		err = -ENODEV;
		goto fail;
	}

	if (!td_flag_test(driver->state, TD_DRIVER_OPEN)) {
		err = -EBADF;
		goto fail;
	}

	if (!driver->ops->td_queue_read) {
		err = -EOPNOTSUPP;
		goto fail;
	}

	err = tapdisk_image_check_td_request(image, treq);
	if (err)
		goto fail;

	driver->ops->td_queue_read(driver, treq);

	return;

fail:
	td_complete_request(treq, err);
}

void
td_forward_request(td_request_t treq)
{
	tapdisk_vbd_forward_request(treq);
}

void
td_complete_request(td_request_t treq, int res)
{
	treq.cb(treq, res);
}

void
td_queue_tiocb(td_driver_t *driver, struct tiocb *tiocb)
{
	tapdisk_driver_queue_tiocb(driver, tiocb);
}

void
td_prep_read(struct tiocb *tiocb, int fd, char *buf, size_t bytes,
	     long long offset, td_queue_callback_t cb, void *arg)
{
	tapdisk_prep_tiocb(tiocb, fd, 0, buf, bytes, offset, cb, arg);
}

void
td_prep_write(struct tiocb *tiocb, int fd, char *buf, size_t bytes,
	      long long offset, td_queue_callback_t cb, void *arg)
{
	tapdisk_prep_tiocb(tiocb, fd, 1, buf, bytes, offset, cb, arg);
}

void
td_debug(td_image_t *image)
{
	td_driver_t *driver;

	driver = image->driver;
	if (!driver || !td_flag_test(driver->state, TD_DRIVER_OPEN))

		return;

	tapdisk_driver_debug(driver);
}

__noreturn void
td_panic(void)
{
	tlog_precious(1);
	raise(SIGABRT);
	_exit(-1); /* not reached */
}
