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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <glob.h>

#include "tap-ctl.h"
#include "blktap2.h"
#include "list.h"

static tap_list_t*
_tap_list_alloc(void)
{
	const size_t sz = sizeof(tap_list_t);
	tap_list_t *tl;

	tl = malloc(sz);
	if (!tl)
		return NULL;

	tl->pid   = -1;
	tl->minor = -1;
	tl->state = -1;
	tl->type  = NULL;
	tl->path  = NULL;

	INIT_LIST_HEAD(&tl->entry);

	return tl;
}

static void
_tap_list_free(tap_list_t *tl)
{
	list_del_init(&tl->entry);

	if (tl->type) {
		free(tl->type);
		tl->type = NULL;
	}

	if (tl->path) {
		free(tl->path);
		tl->path = NULL;
	}

	free(tl);
}

int
_parse_params(const char *params, char **type, char **path)
{
	char *ptr;
	size_t len;

	ptr = strchr(params, ':');
	if (!ptr)
		return -EINVAL;

	len = ptr - params;

	*type = strndup(params, len);
	*path =  strdup(params + len + 1);

	if (!*type || !*path) {
		free(*type);
		*type = NULL;

		free(*path);
		*path = NULL;

		return -errno;
	}

	return 0;
}

void
tap_ctl_list_free(struct list_head *list)
{
	tap_list_t *tl, *n;

	tap_list_for_each_entry_safe(tl, n, list)
		_tap_list_free(tl);
}

static int
_tap_ctl_find_minors(struct list_head *list)
{
	const char *pattern, *format;
	glob_t glbuf = { 0 };
	tap_list_t *tl;
	int i, err;

	INIT_LIST_HEAD(list);

	pattern = BLKTAP2_SYSFS_DIR"/blktap*";
	format  = BLKTAP2_SYSFS_DIR"/blktap%d";

	err = glob(pattern, 0, NULL, &glbuf);
	switch (err) {
	case GLOB_NOMATCH:
		goto done;

	case GLOB_ABORTED:
	case GLOB_NOSPACE:
		err = -errno;
		EPRINTF("%s: glob failed, err %d", pattern, err);
		goto fail;
	}

	for (i = 0; i < glbuf.gl_pathc; ++i) {
		int n;

		tl = _tap_list_alloc();
		if (!tl) {
			err = -ENOMEM;
			goto fail;
		}

		n = sscanf(glbuf.gl_pathv[i], format, &tl->minor);
		if (n != 1) {
			_tap_list_free(tl);
			continue;
		}

		list_add_tail(&tl->entry, list);
	}

done:
	err = 0;
out:
	if (glbuf.gl_pathv)
		globfree(&glbuf);

	return err;

fail:
	tap_ctl_list_free(list);
	goto out;
}

int
_tap_ctl_find_tapdisks(struct list_head *list)
{
	const char *pattern, *format;
	glob_t glbuf = { 0 };
	int err, i, n_taps = 0;

	pattern = BLKTAP2_CONTROL_DIR"/"BLKTAP2_CONTROL_SOCKET"*";
	format  = BLKTAP2_CONTROL_DIR"/"BLKTAP2_CONTROL_SOCKET"%d";

	INIT_LIST_HEAD(list);

	err = glob(pattern, 0, NULL, &glbuf);
	switch (err) {
	case GLOB_NOMATCH:
		goto done;

	case GLOB_ABORTED:
	case GLOB_NOSPACE:
		err = -errno;
		EPRINTF("%s: glob failed, err %d", pattern, err);
		goto fail;
	}

	for (i = 0; i < glbuf.gl_pathc; ++i) {
		tap_list_t *tl;
		int n;

		tl = _tap_list_alloc();
		if (!tl) {
			err = -ENOMEM;
			goto fail;
		}

		n = sscanf(glbuf.gl_pathv[i], format, &tl->pid);
		if (n != 1)
			goto skip;

		tl->pid = tap_ctl_get_pid(tl->pid);
		if (tl->pid < 0)
			goto skip;

		list_add_tail(&tl->entry, list);
		n_taps++;
		continue;

skip:
		_tap_list_free(tl);
	}

done:
	err = 0;
out:
	if (glbuf.gl_pathv)
		globfree(&glbuf);

	return err ? : n_taps;

fail:
	tap_ctl_list_free(list);
	goto out;
}

int
_tap_ctl_list_tapdisk(pid_t pid, struct list_head *list)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
	tapdisk_message_t message;
	tap_list_t *tl;
	int err, sfd;

	err = tap_ctl_connect_id(pid, &sfd);
	if (err)
		return err;

	memset(&message, 0, sizeof(message));
	message.type   = TAPDISK_MESSAGE_LIST;
	message.cookie = -1;

	err = tap_ctl_write_message(sfd, &message, &timeout);
	if (err)
		return err;

	INIT_LIST_HEAD(list);

	do {
		err = tap_ctl_read_message(sfd, &message, &timeout);
		if (err) {
			err = -EPROTO;
			goto fail;
		}

		if (message.u.list.count == 0)
			break;

		tl = _tap_list_alloc();
		if (!tl) {
			err = -ENOMEM;
			goto fail;
		}

		tl->pid    = pid;
		tl->minor  = message.u.list.minor;
		tl->state  = message.u.list.state;

		if (message.u.list.path[0] != 0) {
			err = _parse_params(message.u.list.path,
					    &tl->type, &tl->path);
			if (err) {
				_tap_list_free(tl);
				goto fail;
			}
		}

		list_add(&tl->entry, list);
	} while (1);

	err = 0;
out:
	close(sfd);
	return 0;

fail:
	tap_ctl_list_free(list);
	goto out;
}

int
tap_ctl_list(struct list_head *list)
{
	struct list_head minors, tapdisks, vbds;
	tap_list_t *t, *next_t, *v, *next_v, *m, *next_m;
	int err;

	/*
	 * Find all minors, find all tapdisks, then list all minors
	 * they attached to. Output is a 3-way outer join.
	 */

	err = _tap_ctl_find_minors(&minors);
	if (err < 0)
		goto fail;

	err = _tap_ctl_find_tapdisks(&tapdisks);
	if (err < 0)
		goto fail;

	INIT_LIST_HEAD(list);

	tap_list_for_each_entry_safe(t, next_t, &tapdisks) {

		err = _tap_ctl_list_tapdisk(t->pid, &vbds);

		if (err || list_empty(&vbds)) {
			list_move_tail(&t->entry, list);
			continue;
		}

		tap_list_for_each_entry_safe(v, next_v, &vbds) {

			tap_list_for_each_entry_safe(m, next_m, &minors)
				if (m->minor == v->minor) {
					_tap_list_free(m);
					break;
				}

			list_move_tail(&v->entry, list);
		}

		_tap_list_free(t);
	}

	/* orphaned minors */
	list_splice_tail(&minors, list);

	return 0;

fail:
	tap_ctl_list_free(list);

	tap_ctl_list_free(&vbds);
	tap_ctl_list_free(&tapdisks);
	tap_ctl_list_free(&minors);

	return err;
}

int
tap_ctl_list_pid(pid_t pid, struct list_head *list)
{
	tap_list_t *t;
	int err;

	t = _tap_list_alloc();
	if (!t)
		return -ENOMEM;

	t->pid = tap_ctl_get_pid(pid);
	if (t->pid < 0) {
		_tap_list_free(t);
		return 0;
	}

	err = _tap_ctl_list_tapdisk(t->pid, list);

	if (err || list_empty(list))
		list_add_tail(&t->entry, list);

	return 0;
}

int
tap_ctl_find_minor(const char *type, const char *path)
{
	struct list_head list = LIST_HEAD_INIT(list);
	tap_list_t *entry;
	int minor, err;

	err = tap_ctl_list(&list);
	if (err)
		return err;

	minor = -1;

	tap_list_for_each_entry(entry, &list) {

		if (type && (!entry->type || strcmp(entry->type, type)))
			continue;

		if (path && (!entry->path || strcmp(entry->path, path)))
			continue;

		minor = entry->minor;
		break;
	}

	tap_ctl_list_free(&list);

	return minor >= 0 ? minor : -ENOENT;
}
