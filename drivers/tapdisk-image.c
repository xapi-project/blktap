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
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <regex.h>

#include "tapdisk-image.h"
#include "tapdisk-driver.h"
#include "tapdisk-server.h"
#include "tapdisk-stats.h"
#include "tapdisk-interface.h"
#include "tapdisk-disktype.h"
#include "tapdisk-storage.h"
#include "util.h"

#define DBG(_f, _a...)       tlog_syslog(TLOG_DBG, _f, ##_a)
#define INFO(_f, _a...)      tlog_syslog(TLOG_INFO, _f, ##_a)
#define ERR(_err, _f, _a...) tlog_error(_err, _f, ##_a)

#define BUG() td_panic()

#define BUG_ON(_cond)						\
	if (unlikely(_cond)) {					\
		ERR(-EINVAL, "(%s) = %d", #_cond, _cond);	\
		BUG();						\
	}

td_image_t *
tapdisk_image_allocate(const char *file, int type, td_flag_t flags)
{
	int err;
	td_image_t *image;

	image = calloc(1, sizeof(td_image_t));
	if (!image)
		return NULL;

	err = tapdisk_namedup(&image->name, file);
	if (err) {
		free(image);
		return NULL;
	}

	image->type    = type;
	image->flags   = flags;
	INIT_LIST_HEAD(&image->next);

	return image;
}

void
tapdisk_image_free(td_image_t *image)
{
	if (!image)
		return;

	list_del(&image->next);

	free(image->name);
	tapdisk_driver_free(image->driver);
	free(image);
}

int
tapdisk_image_check_td_request(td_image_t *image, td_request_t treq)
{
	int rdonly, err;
	td_disk_info_t *info;

	err = -EINVAL;

	info   = &image->info;
	rdonly = td_flag_test(image->flags, TD_OPEN_RDONLY);

	if (treq.op != TD_OP_READ && treq.op != TD_OP_WRITE)
		goto fail;

	if (treq.op == TD_OP_WRITE && rdonly) {
		err = -EPERM;
		goto fail;
	}

	if (treq.secs <= 0 || treq.sec + treq.secs > info->size)
		goto fail;

	return 0;

fail:
	ERR(err, "bad td request on %s (%s, %"PRIu64"): %d at %"PRIu64,
	    image->name, (rdonly ? "ro" : "rw"), info->size, treq.op,
	    treq.sec + treq.secs);
	return err;

}

int
tapdisk_image_check_request(td_image_t *image, td_vbd_request_t *vreq)
{
	td_driver_t *driver;
	td_disk_info_t *info;
	int i, rdonly, secs, err;

	driver = image->driver;
	if (!driver)
		return -ENODEV;

	info   = &driver->info;
	rdonly = td_flag_test(image->flags, TD_OPEN_RDONLY);

	secs = 0;

	if (vreq->iovcnt < 0) {
		err = -EINVAL;
		goto fail;
	}

	for (i = 0; i < vreq->iovcnt; i++)
		secs += vreq->iov[i].secs;

	switch (vreq->op) {
	case TD_OP_WRITE:
		if (rdonly) {
			err = -EPERM;
			goto fail;
		}
		/* continue */
	case TD_OP_READ:
		if (vreq->sec + secs > info->size) {
			err = -EINVAL;
			goto fail;
		}
		break;
	default:
		err = -EOPNOTSUPP;
		goto fail;
	}

	return 0;

fail:
	ERR(err, "bad request on %s (%s, %"PRIu64"): req %s op %d at %"PRIu64,
	    image->name, (rdonly ? "ro" : "rw"), info->size, vreq->name,
	    vreq->op, vreq->sec + secs);

	return err;
}

void
tapdisk_image_close(td_image_t *image)
{
	td_close(image);
	tapdisk_image_free(image);
}

int
tapdisk_image_open(int type, const char *name, int flags, td_image_t **_image)
{
	td_image_t *image;
	int err;

	image = tapdisk_image_allocate(name, type, flags);
	if (!image) {
		err = -ENOMEM;
		goto fail;
	}

	err = td_load(image);
	if (!err)
		goto done;

	image->driver = tapdisk_driver_allocate(image->type,
						image->name,
						image->flags);
	if (!image->driver) {
		err = -ENOMEM;
		goto fail;
	}

	err = td_open(image);
	if (err)
		goto fail;

done:
	*_image = image;
	return 0;

fail:
	if (image)
		tapdisk_image_close(image);
	return err;
}

static int
tapdisk_image_open_parent(td_image_t *image, td_image_t **_parent)
{
	td_image_t *parent = NULL;
	td_disk_id_t id;
	int err;

	memset(&id, 0, sizeof(id));
	id.flags = image->flags;

	err = td_get_parent_id(image, &id);
	if (err == TD_NO_PARENT) {
		err = 0;
		goto out;
	}
	if (err)
		return err;

    if (((id.flags & TD_OPEN_NO_O_DIRECT) == TD_OPEN_NO_O_DIRECT) &&
            ((id.flags & TD_OPEN_LOCAL_CACHE) == TD_OPEN_LOCAL_CACHE))
        id.flags &= ~TD_OPEN_NO_O_DIRECT;
	err = tapdisk_image_open(id.type, id.name, id.flags, &parent);
	if (err)
		return err;

out:
	*_parent = parent;
	return 0;
}

static int
tapdisk_image_open_parents(td_image_t *image)
{
	td_image_t *parent;
	int err;

	do {
		err = tapdisk_image_open_parent(image, &parent);
		if (err)
			break;

		if (parent) {
			list_add(&parent->next, &image->next);
			image = parent;
		}
	} while (parent);

	return err;
}

void
tapdisk_image_close_chain(struct list_head *list)
{
	td_image_t *image, *next;

	tapdisk_for_each_image_safe(image, next, list)
		tapdisk_image_close(image);
}

/**
 * Opens the image and all of its parents.
 *
 * @param type DISK_TYPE_* (see tapdisk-disktype.h)
 * @param name /path/to/file
 * @param flags
 * @param _head
 * @param prt_devnum parent minor (optional)
 * @returns
 */
static int
__tapdisk_image_open_chain(int type, const char *name, int flags,
			   struct list_head *_head, int prt_devnum)
{
	struct list_head head = LIST_HEAD_INIT(head);
	td_image_t *image;
	int err;

	err = tapdisk_image_open(type, name, flags, &image);
	if (err)
		goto fail;

	list_add_tail(&image->next, &head);

	if (unlikely(prt_devnum >= 0)) {
		char dev[32];
		snprintf(dev, sizeof(dev),
			 "%s%d", BLKTAP2_IO_DEVICE, prt_devnum);
		err = tapdisk_image_open(DISK_TYPE_AIO, dev,
					 flags|TD_OPEN_RDONLY, &image);
		if (err)
			goto fail;

		list_add_tail(&image->next, &head);
		goto done;
	}

	err = tapdisk_image_open_parents(image);
	if (err)
		goto fail;

done:
	list_splice(&head, _head);
	return 0;

fail:
	tapdisk_image_close_chain(&head);
	return err;
}

int
tapdisk_image_parse_flags(char *args, unsigned long *_flags)
{
	unsigned long flags = 0;
	char *token;

	BUG_ON(!args);

	do {
		token = strtok(args, ",");
		if (!token)
			break;

		switch (token[0]) {
		case 'r':
			if (!strcmp(token, "ro")) {
				flags |= TD_OPEN_RDONLY;
				break;
			}
			goto fail;

		default:
			goto fail;
		}

		args = NULL;
	} while (1);

	*_flags |= flags;

	return 0;

fail:
	ERR(-EINVAL, "Invalid token '%s'", token);
	return -EINVAL;
}

static int
tapdisk_image_open_x_chain(const char *path, struct list_head *_head)
{
	struct list_head head = LIST_HEAD_INIT(head);
	td_image_t *image = NULL, *next;
	regex_t _im, *im = NULL, _ws, *ws = NULL;
	FILE *s;
	int err;

	s = fopen(path, "r");
	if (!s) {
		err = -errno;
		goto fail;
	}

	err = regcomp(&_ws, "^[:space:]*$", REG_NOSUB);
	if (err)
		goto fail;
	ws = &_ws;

	err = regcomp(&_im,
		      "^([^:]+):([^ \t]+)([ \t]+([a-z,]+))?",
		      REG_EXTENDED|REG_NEWLINE);
	if (err)
		goto fail;
	im = &_im;

	do {
		char line[512], *l;
		regmatch_t match[5];
		char *typename, *path, *args = NULL;
		unsigned long flags;
		int type;

		l = fgets(line, sizeof(line), s);
		if (!l)
			break;

		err = regexec(im, line, ARRAY_SIZE(match), match, 0);
		if (err) {
			err = regexec(ws, line, ARRAY_SIZE(match), match, 0);
			if (!err)
				continue;
			err = -EINVAL;
			goto fail;
		}

		line[match[1].rm_eo] = 0;
		typename = line + match[1].rm_so;

		line[match[2].rm_eo] = 0;
		path = line + match[2].rm_so;

		if (match[4].rm_so >= 0) {
			line[match[4].rm_eo] = 0;
			args = line + match[4].rm_so;
		}

		type = tapdisk_disktype_find(typename);
		if (type < 0) {
			err = type;
			goto fail;
		}

		flags = 0;

		if (args) {
			err = tapdisk_image_parse_flags(args, &flags);
			if (err)
				goto fail;
		}

		err = tapdisk_image_open(type, path, flags, &image);
		if (err)
			goto fail;

		list_add_tail(&image->next, &head);
	} while (1);

	if (!image) {
		err = -EINVAL;
		goto fail;
	}

	err = tapdisk_image_open_parents(image);
	if (err)
		goto fail;

	list_splice(&head, _head);
out:
	if (im)
		regfree(im);
	if (ws)
		regfree(ws);
	if (s)
		fclose(s);

	return err;

fail:
	tapdisk_for_each_image_safe(image, next, &head)
		tapdisk_image_free(image);

	goto out;
}

int
tapdisk_image_open_chain(const char *desc, int flags, int prt_devnum,
			 struct list_head *head)
{
	const char *name;
	int type, err;

	type = tapdisk_disktype_parse_params(desc, &name);
	if (type >= 0)
		return __tapdisk_image_open_chain(type, name, flags, head,
						  prt_devnum);

	err = type;

	if (err == -ENOENT && strlen(desc) >= 3) {
		switch (desc[2]) {
		case 'c':
			if (!strncmp(desc, "x-chain", strlen("x-chain")))
				err = tapdisk_image_open_x_chain(name, head);
			break;
		}
	}

	return err;
}

int
tapdisk_image_validate_chain(struct list_head *head)
{
	td_image_t *image, *parent;
	int flags, err;

	INFO("VBD CHAIN:\n");

	tapdisk_for_each_image_reverse(parent, head) {
		image = tapdisk_image_entry(parent->next.prev);

		if (image == tapdisk_image_entry(head))
			break;

		err = td_validate_parent(image, parent);
		if (err)
			return err;

		flags = tapdisk_disk_types[image->type]->flags;
		if (flags & DISK_TYPE_FILTER) {
			image->driver->info = parent->driver->info;
			image->info         = parent->info;
		}
	}

	tapdisk_for_each_image(image, head) {
		INFO("%s: type:%s(%d) storage:%s(%d)\n",
		     image->name,
		     tapdisk_disk_types[image->type]->name,
		     image->type,
		     tapdisk_storage_name(image->driver->storage),
		     image->driver->storage);
	}

	return 0;
}

void
tapdisk_image_stats(td_image_t *image, td_stats_t *st)
{
	tapdisk_stats_enter(st, '{');
	tapdisk_stats_field(st, "name", "s", image->name);

	tapdisk_stats_field(st, "hits", "[");
	tapdisk_stats_val(st, "llu", image->stats.hits.rd);
	tapdisk_stats_val(st, "llu", image->stats.hits.wr);
	tapdisk_stats_leave(st, ']');

	tapdisk_stats_field(st, "fail", "[");
	tapdisk_stats_val(st, "llu", image->stats.fail.rd);
	tapdisk_stats_val(st, "llu", image->stats.fail.wr);
	tapdisk_stats_leave(st, ']');

	tapdisk_stats_field(st, "driver", "{");
	tapdisk_driver_stats(image->driver, st);
	tapdisk_stats_leave(st, '}');

	tapdisk_stats_leave(st, '}');
}
