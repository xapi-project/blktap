/*
 * Copyright (c) 2005 Julian Chesterfield and Andrew Warfield.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <xs.h>
#include <xen/io/xenbus.h>
                                                                     
#include "tapdisk-dispatch.h"

#include "xs_api.h"
#include "disktypes.h"

#define MAX_ATTEMPTS         10
#define PIDFILE              "/var/run/blktapctrl.pid"

#define DAEMON_WATCH_DOMID   "tapdisk-watch-domid"
#define DAEMON_WATCH_VBD     "tapdisk-watch-vbd"

static int run = 1;
static int blktap_ctlfd;
static unsigned long tapdisk_uuid;

static void
print_drivers(void)
{
	int i, size;

	DPRINTF("blktap-daemon: v1.0.1\n");

	size = sizeof(dtypes) / sizeof(disk_info_t *);
	for (i = 0; i < size; i++)
		DPRINTF("Found driver: [%s]\n", dtypes[i]->name);
} 

static void
write_pidfile(long pid)
{
	char buf[100];
	int len;
	int fd;
	int flags;

	fd = open(PIDFILE, O_RDWR | O_CREAT, 0600);
	if (fd == -1) {
		EPRINTF("Opening pid file failed (%d)\n", errno);
		exit(1);
	}

	/* We exit silently if daemon already running */
	if (lockf(fd, F_TLOCK, 0) == -1)
		exit(0);

	/* Set FD_CLOEXEC, so that tapdisk doesn't get this file descriptor */
	if ((flags = fcntl(fd, F_GETFD)) == -1) {
		EPRINTF("F_GETFD failed (%d)\n", errno);
		exit(1);
	}
	flags |= FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, flags) == -1) {
		EPRINTF("F_SETFD failed (%d)\n", errno);
		exit(1);
	}

	len = sprintf(buf, "%ld\n", pid);
	if (write(fd, buf, len) != len) {
		EPRINTF("Writing pid file failed (%d)\n", errno);
		exit(1);
	}
}

/*
 * ignore individual VBD property events --
 * they'll be handled by a tapdisk-control process
 */
static int
daemon_new_vbd_event(struct xs_handle *h, const char *node)
{
	return (!strcmp(node, "start-tapdisk"));
}

static char *
tapdisk_write_uuid(struct xs_handle *h, char *path)
{
	char *uuid, *upath;

	uuid  = NULL;
	upath = NULL;

	if (asprintf(&uuid, "%lu", tapdisk_uuid++) == -1) {
		EPRINTF("failed to allocate new tapdisk-uuid\n");
		return NULL;
	}

	if (asprintf(&upath, "%s/tapdisk-uuid", path) == -1) {
		EPRINTF("failed to allocate tapdisk-uuid path\n");
		upath = NULL;
		goto fail;
	}

	if (!xs_write(h, XBT_NULL, upath, uuid, strlen(uuid) + 1)) {
		EPRINTF("failed to write tapdisk-uuid\n");
		goto fail;
	}

	free(upath);
	return uuid;

 fail:
	free(uuid);
	free(upath);
	return NULL;
}

/*
 * fork new process to handle individual VBD property events
 */
static int
start_tapdisk_controller(struct xs_handle *h, char *path)
{
	int err;
	pid_t child;
	char *uuid, *argv[] = { "tapdisk-control", path, NULL, NULL };

	err  = -1;
	uuid = tapdisk_write_uuid(h, path);
	if (!uuid)
		goto out;
	argv[2] = uuid;

	if ((child = fork()) < 0) {
		EPRINTF("failed to fork tapdisk-control\n");
		goto out;
	}

	if (!child) {
		int i;

		for (i = 0; i < sysconf(_SC_OPEN_MAX); i++)
			if (i != STDIN_FILENO &&
			    i != STDOUT_FILENO &&
			    i != STDERR_FILENO)
				close(i);

		DPRINTF("launching tapdisk-control[%d] %s %s\n",
			getpid(), path, uuid);

		execvp("tapdisk-control", argv);
		_exit(1);
	} else {
		pid_t got;
		do {
			got = waitpid(child, NULL, 0);
		} while (got != child);
	}

	err = 0;

 out:
	free(uuid);
	free(path);
	return 0;
}

/*
 * Xenstore watch callback entry point. This code replaces the hotplug scripts,
 * and as soon as the xenstore backend driver entries are created, this script
 * gets called.
 */
static void
tapdisk_daemon_probe(struct xs_handle *h, const char *path)
{
	int len;
	const char *node;

	len = strsep_len(path, '/', 7);
	if (len < 0)
		return;

	node = path + len + 1;

	if (daemon_new_vbd_event(h, node)) {
		char *cpath;

		if (!xs_exists(h, path))
			return;

	       	cpath = strdup(path);
		if (!cpath) {
			EPRINTF("ERROR: failed to allocate control "
				"path for %s\n", path);
			return;
		}
		cpath[len] = '\0';

		start_tapdisk_controller(h, cpath);
	}
}

/*
 * We set a general watch on the backend tap directory and spawn
 * tapdisk-control processes when we receive requests for new VBDs.
 */
static int
add_daemon_watch(struct xs_handle *h, const char *domid)
{
	char *path;
	int err = 0;

	if (asprintf(&path, "/local/domain/%s/backend/tap", domid) == -1)
		return -ENOMEM;

	if (!xs_watch(h, path, DAEMON_WATCH_VBD)) {
		EPRINTF("unable to set vbd watch\n");
		err = -EINVAL;
	}

 out:
	free(path);
	return err;
}

static void
remove_daemon_watch(struct xs_handle *h)
{
	char *path, *domid;

	domid = get_dom_domid(h);
	if (!domid)
		return;

	if (asprintf(&path, "/local/domain/%s/backend/tap", domid) != -1) {
		xs_unwatch(h, path, DAEMON_WATCH_VBD);
		free(path);
	}

	free(domid);
}

/* Asynch callback to check for /local/domain/<DOMID>/name */
static void
check_dom(struct xs_handle *h, const char *bepath_im)
{
	char *domid;

	domid = get_dom_domid(h);
	if (domid == NULL)
		return;

	add_daemon_watch(h, domid);
	free(domid);
	xs_unwatch(h, "/local/domain", DAEMON_WATCH_DOMID);
}

/* We must wait for xend to register /local/domain/<DOMID> */
static int
watch_for_domid(struct xs_handle *h)
{
	if (!xs_watch(h, "/local/domain", DAEMON_WATCH_DOMID)) {
		EPRINTF("unable to set domain id watch\n");
		return -EINVAL;
	}

	return 0;
}

static int
setup_daemon_watch(struct xs_handle *h)
{
	char *domid;
	int ret;
	
	domid = get_dom_domid(h);
	if (!domid)
		return watch_for_domid(h);

	ret = add_daemon_watch(h, domid);
	free(domid);
	return ret;
}

static int
tapdisk_daemon_handle_event(struct xs_handle *h)
{
	int ret;
	unsigned int num;
	char **res, *node, *token;

	res = xs_read_watch(h, &num);
	if (!res)
		return -EAGAIN;

	ret   = 0;
	node  = res[XS_WATCH_PATH];
	token = res[XS_WATCH_TOKEN];

	if (!strcmp(token, DAEMON_WATCH_DOMID))
		check_dom(h, node);
	else if (!strcmp(token, DAEMON_WATCH_VBD))
		tapdisk_daemon_probe(h, node);
	else
		ret = -EINVAL;

	free(res);
	return ret;
}

int
main(int argc, char *argv[])
{
	int ret;
	int count;
	char buf[128];
	char *devname;
	pid_t process;
	fd_set readfds;
	int blktap_major;
	struct xs_handle *xsh;

	daemon(0, 0);

	count   = 0;
	process = getpid();

	write_pidfile(process);

	snprintf(buf, sizeof(buf), "BLKTAP-DAEMON[%d]", process);
	openlog(buf, LOG_CONS | LOG_ODELAY, LOG_DAEMON);

	print_drivers();

	/* Attach to blktap0 */
	ret = asprintf(&devname, "%s/%s0", BLKTAP_DEV_DIR, BLKTAP_DEV_NAME);
	if (ret < 0)
		goto open_failed;

	ret = xc_find_device_number("blktap0");
	if (ret < 0)
		goto open_failed;

	blktap_major = major(ret);
	make_blktap_dev(devname, blktap_major, 0, S_IFCHR | 0600);

	blktap_ctlfd = open(devname, O_RDWR);
	if (blktap_ctlfd == -1) {
		EPRINTF("blktap0 open failed\n");
		goto open_failed;
	}

 retry:
	/* Set up store connection and watch. */
	xsh = xs_daemon_open();
	if (xsh == NULL) {
		EPRINTF("xs_daemon_open failed -- "
			"is xenstore running?\n");
		if (count < MAX_ATTEMPTS) {
			count++;
			sleep(2);
			goto retry;
		} else 
			goto open_failed;
	}

	ret = setup_daemon_watch(xsh);
	if (ret != 0) {
		EPRINTF("Failed adding device probewatch\n");
		xs_daemon_close(xsh);
		goto open_failed;
	}

	ioctl(blktap_ctlfd, BLKTAP_IOCTL_SETMODE, BLKTAP_MODE_INTERPOSE);
	ioctl(blktap_ctlfd, BLKTAP_IOCTL_SENDPID, process);

	while (run) {
		FD_ZERO(&readfds);
		FD_SET(xs_fileno(xsh), &readfds);

		ret = select(xs_fileno(xsh) + 1, &readfds, NULL, NULL, NULL);

		if (FD_ISSET(xs_fileno(xsh), &readfds))
			ret = tapdisk_daemon_handle_event(xsh);
	}

	remove_daemon_watch(xsh);
	xs_daemon_close(xsh);

	ioctl(blktap_ctlfd, BLKTAP_IOCTL_SETMODE, BLKTAP_MODE_PASSTHROUGH);
	close(blktap_ctlfd);

	closelog();

	return 0;
	
 open_failed:
	EPRINTF("Unable to start blktap-daemon\n");
	closelog();
	return -1;
}

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
