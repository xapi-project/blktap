/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 * This file contains the core of the tapback daemon, the user space daemon
 * that acts as a device's back-end.
 */

/*
 * TODO Some of these includes may be useless.
 * TODO Replace hard-coding strings with defines/const string.
 */
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <syslog.h>

#include "blktap3.h"
#include "stdio.h" /* TODO tap-ctl.h needs to include stdio.h */
#include "tap-ctl.h"
#include "tapback.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

void tapback_log(int prio, const char *fmt, ...);
void (*tapback_vlog) (int prio, const char *fmt, va_list ap);

struct _blktap3_daemon blktap3_daemon;

char *XenbusState2str(const XenbusState xbs)
{
    static char * const str[] = {
        [XenbusStateUnknown] = "0 (unknown)",
        [XenbusStateInitialising] = "1 (initialising)",
        [XenbusStateInitWait] = "2 (init wait)",
        [XenbusStateInitialised] = "3 (initialised)",
        [XenbusStateConnected] = "4 (connected)",
        [XenbusStateClosing] = "5 (closing)",
        [XenbusStateClosed] = "6 (closed)",
        [XenbusStateReconfiguring] = "7 (reconfiguring)",
        [XenbusStateReconfigured] = "8 (reconfigured)"
    };
    return str[xbs];
}

/**
 * Read changes that occurred on the "backend/<backend name>" XenStore path
 * or one of the front-end paths and act accordingly.
 */
static inline void
tapback_read_watch(void)
{
    char **watch = NULL, *path = NULL, *token = NULL;
    unsigned int n = 0;
    int err = 0, _abort = 0;

    /* read the change */
    watch = xs_read_watch(blktap3_daemon.xs, &n);
    path = watch[XS_WATCH_PATH];
    token = watch[XS_WATCH_TOKEN];

    /*
     * TODO Put the body of "again:" into a loop instead of using goto.
     */
again:
    if (!(blktap3_daemon.xst = xs_transaction_start(blktap3_daemon.xs))) {
        WARN(NULL, "error starting transaction\n");
        goto fail;
    }

	{
		char *s = tapback_xs_read(blktap3_daemon.xs, blktap3_daemon.xst, "%s",
				path);
	    DBG(NULL, "%s -> \'%s\'\n", path, s ? s : "removed");
		free(s);
	}

    /*
     * The token indicates which XenStore watch triggered, the front-end one or
     * the back-end one.
     */
    if (!strcmp(token, BLKTAP3_FRONTEND_TOKEN)) {
        err = tapback_backend_handle_otherend_watch(path);
    } else if (!strcmp(token, BLKTAP3_BACKEND_TOKEN)) {
        err = -tapback_backend_handle_backend_watch(path);
	} else if(!strcmp(token, FORCED_HVM_SHUTDOWN_TOKEN)) {

    } else {
        WARN(NULL, "invalid token \'%s\'\n", token);
        err = EINVAL;
    }

    _abort = !!err;
    if (_abort) {
        if (err != ENOENT) {
            /* TODO Some functions return +err, others -err */
            DBG(NULL, "aborting transaction: %s\n", strerror(abs(err)));
        }
    }

    err = xs_transaction_end(blktap3_daemon.xs, blktap3_daemon.xst, _abort);
    blktap3_daemon.xst = 0;
    if (!err) {
        err = -errno;
        /*
         * This is OK according to xs_transaction_end's semantics.
		 *
		 * FIXME We should undo whatever we did as something might have
		 * changed. This is very difficult.
         */
        if (EAGAIN == errno) {
			DBG(NULL, "restarting transaction\n");
            goto again;
		}
        DBG(NULL, "error ending transaction: %s\n", strerror(err));
    }

fail:
    free(watch);
    return;
}

static void
tapback_backend_destroy(void)
{
    int err;

    if (blktap3_daemon.xs) {
        xs_daemon_close(blktap3_daemon.xs);
        blktap3_daemon.xs = NULL;
    }

    err = unlink(TAPBACK_CTL_SOCK_PATH);
    if (err == -1 && errno != ENOENT) {
        err = errno;
        WARN(NULL, "failed to remove %s: %s\n", TAPBACK_CTL_SOCK_PATH,
				strerror(err));
    }
}

static void
signal_cb(int signum) {

    ASSERT(signum == SIGINT || signum == SIGTERM);

	if (!list_empty(&blktap3_daemon.devices)) {
		WARN(NULL, "refusing to shutdown while there are active VBDs\n");
		return;
	}

    tapback_backend_destroy();
    exit(0);
}

/**
 * Initializes the back-end descriptor. There is one back-end per tapback
 * process. Also, it initiates a watch to XenStore on backend/<backend name>.
 *
 * @returns 0 on success, an error code otherwise
 */
static inline int
tapback_backend_create(void)
{
    int err;
    struct sockaddr_un local;
    int len;

    INIT_LIST_HEAD(&blktap3_daemon.devices);
    blktap3_daemon.xst = XBT_NULL;
    blktap3_daemon.ctrl_sock = -1;

    if (!(blktap3_daemon.xs = xs_daemon_open())) {
        err = EINVAL;
        goto fail;
    }

    /*
     * Watch the back-end.
     */
    if (!xs_watch(blktap3_daemon.xs, BLKTAP3_BACKEND_PATH,
                BLKTAP3_BACKEND_TOKEN)) {
        err = errno;
        goto fail;
    }

    if (SIG_ERR == signal(SIGINT, signal_cb) ||
            SIG_ERR == signal(SIGTERM, signal_cb)) {
        WARN(NULL, "failed to register signal handlers\n");
        err = EINVAL;
        goto fail;
    }

    /*
     * Create the control socket.
     * XXX We don't listen for connections as we don't yet support any control
     * commands.
     */
    blktap3_daemon.ctrl_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (blktap3_daemon.ctrl_sock == -1) {
        err = errno;
        WARN(NULL, "failed to create control socket: %s\n", strerror(errno));
        goto fail;
    }
    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, TAPBACK_CTL_SOCK_PATH);
    err = unlink(local.sun_path);
    if (err && errno != ENOENT) {
        err = errno;
        WARN(NULL, "failed to remove %s: %s\n", local.sun_path, strerror(err));
        goto fail;
    }
    len = strlen(local.sun_path) + sizeof(local.sun_family);
    err = bind(blktap3_daemon.ctrl_sock, (struct sockaddr *)&local, len);
    if (err == -1) {
        err = errno;
        WARN(NULL, "failed to bind to %s: %s\n", local.sun_path, strerror(err));
        goto fail;
    }

    return 0;

fail:
	tapback_backend_destroy();

    return err;
}

/**
 * Runs the daemon.
 *
 * Watches backend/<backend name> and the front-end devices.
 */
static inline int
tapback_backend_run(void)
{
    const int fd = xs_fileno(blktap3_daemon.xs);
	int err;

    do {
        fd_set rfds;
        int nfds = 0;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        /* poll the fd for changes in the XenStore path we're interested in */
        if ((nfds = select(fd + 1, &rfds, NULL, NULL, NULL)) < 0) {
            perror("error monitoring XenStore");
            err = -errno;
            break;
        }

        if (FD_ISSET(fd, &rfds))
            tapback_read_watch();
        DBG(NULL, "--\n");
    } while (1);

    return err;
}

static char *blkback_ident = NULL;

static void
blkback_vlog_fprintf(const int prio, const char * const fmt, va_list ap)
{
    static const char *strprio[] = {
        [LOG_DEBUG] = "DBG",
        [LOG_INFO] = "INF",
        [LOG_WARNING] = "WRN"
    };

    assert(LOG_DEBUG == prio || LOG_INFO == prio || LOG_WARNING == prio);
    assert(strprio[prio]);

    fprintf(stderr, "%s[%s] ", blkback_ident, strprio[prio]);
    vfprintf(stderr, fmt, ap);
}

/**
 * Print tapback's usage instructions.
 */
static void
usage(FILE * const stream, const char * const prog)
{
    ASSERT(stream);
    ASSERT(prog);

    fprintf(stream,
            "usage: %s\n"
            "\t[-D|--debug]\n"
			"\t[-h|--help]\n", prog);
}

int main(int argc, char **argv)
{
    const char *prog = NULL;
    int opt_debug = 0;
    int err = 0;

	if(access("/dev/xen/gntdev", F_OK ) == -1) {
		WARN(NULL, "grant device does not exist\n");
		err = EINVAL;
		goto fail;
	}

    prog = basename(argv[0]);

    opt_debug = 0;

    do {
        const struct option longopts[] = {
            {"help", 0, NULL, 'h'},
            {"debug", 0, NULL, 'D'},
        };
        int c;

        c = getopt_long(argc, argv, "h:D", longopts, NULL);
        if (c < 0)
            break;

        switch (c) {
        case 'h':
            usage(stdout, prog);
            return 0;
        case 'D':
            opt_debug = 1;
            break;
        case '?':
            goto usage;
        }
    } while (1);

    if (opt_debug) {
        blkback_ident = "";
        tapback_vlog = blkback_vlog_fprintf;
    }
    else {
        blkback_ident = BLKTAP3_BACKEND_TOKEN;
        openlog(blkback_ident, 0, LOG_DAEMON);
        setlogmask(LOG_UPTO(LOG_INFO));
    }

    if (!opt_debug) {
        if ((err = daemon(0, 0))) {
            err = -errno;
            goto fail;
        }
    }

	if ((err = tapback_backend_create())) {
        WARN(NULL, "error creating blkback: %s\n", strerror(err));
        goto fail;
    }

    err = tapback_backend_run();

    tapback_backend_destroy();

fail:
    return err ? -err : 0;

usage:
    usage(stderr, prog);
    return 1;
}
