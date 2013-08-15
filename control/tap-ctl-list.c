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

	tl->uuid[0] = '\0';
	tl->pid     = -1;
	tl->state   = -1;
	tl->type    = NULL;
	tl->path    = NULL;

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
parse_params(const char *params, char **type, char **path)
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

/**
 * Returns a list running tapdisks. tapdisks are searched for by looking for
 * their control socket.
 */
static int
_tap_ctl_find_tapdisks(struct list_head *list)
{
	glob_t glbuf = { 0 };
	const char *pattern, *format;
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

/**
 * Retrieves all the VBDs a tapdisk is serving.
 *
 * @param pid the process ID of the tapdisk whose VBDs should be retrieved
 * @param list output parameter that receives the list of VBD
 * @returns 0 on success, an error code otherwise
 */
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
		tl->state  = message.u.list.state;

		strcpy(tl->uuid, message.u.list.uuid);

		if (message.u.list.path[0] != 0) {
			err = parse_params(message.u.list.path, &tl->type, &tl->path);
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
	struct list_head tapdisks;
	tap_list_t *t, *next_t;
	int err;

	err = _tap_ctl_find_tapdisks(&tapdisks);
	if (err < 0) {
		EPRINTF("error finding tapdisks: %s\n", strerror(-err));
		goto out;
	}

	INIT_LIST_HEAD(list);

	tap_list_for_each_entry_safe(t, next_t, &tapdisks) {
		struct list_head vbds;

		err = _tap_ctl_list_tapdisk(t->pid, &vbds);

		if (!err)
			list_splice(&vbds, list);
		else
			EPRINTF("failed to get VBDs for tapdisk %d: %s\n", t->pid,
					strerror(-err));
            /* TODO Return the error or just continue? */

		_tap_list_free(t);
	}

	return 0;

out:
	if (err)
		tap_ctl_list_free(list);

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
