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
#include <time.h>

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

char *xenbus_strstate(const XenbusState xbs)
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
    int err = 0;
    char *s;

    /* read the change */
    watch = xs_read_watch(blktap3_daemon.xs, &n);
    path = watch[XS_WATCH_PATH];
    token = watch[XS_WATCH_TOKEN];

    /*
     * print the path the watch triggered on for debug purposes
     *
     * TODO include token
     */
	s = tapback_xs_read(blktap3_daemon.xs, XBT_NULL, "%s", path);
    if (s) {
        if (0 == strlen(s))
            DBG(NULL, "%s -> (created)\n", path);
        else
            DBG(NULL, "%s -> \'%s\'\n", path, s);
        free(s);
    } else
        DBG(NULL, "%s -> (removed)\n", path);

    /*
     * The token indicates which XenStore watch triggered, the front-end one or
     * the back-end one.
     */
    if (!strcmp(token, BLKTAP3_FRONTEND_TOKEN)) {
        err = tapback_backend_handle_otherend_watch(path);
    } else if (!strcmp(token, BLKTAP3_BACKEND_TOKEN)) {
        err = -tapback_backend_handle_backend_watch(path);
    } else {
        WARN(NULL, "invalid token \'%s\'\n", token);
        err = EINVAL;
    }

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

static FILE *tapback_log_fp = NULL;

static void
signal_cb(int signum) {

    ASSERT(signum == SIGINT || signum == SIGTERM || signum == SIGHUP);

    if (signum == SIGHUP) {
        if (tapback_log_fp) {
            fflush(tapback_log_fp);
            fclose(tapback_log_fp);
            tapback_log_fp = fopen(TAPBACK_LOG, "a+");
        }
    } else {
        if (!list_empty(&blktap3_daemon.devices)) {
            WARN(NULL, "refusing to shutdown while there are active VBDs\n");
            return;
        }

        tapback_backend_destroy();
        exit(0);
    }
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
            SIG_ERR == signal(SIGTERM, signal_cb) ||
            SIG_ERR == signal(SIGHUP, signal_cb)) {
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

    INFO(NULL, "tapback (user-space blkback for tapdisk3) daemon started\n");

    do {
        fd_set rfds;
        int nfds = 0;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        /*
         * poll the fd for changes in the XenStore path we're interested in
         *
         * FIXME Add a (e.g. 3 second) time-out in order to periodically
         * flush the log.
         */
        nfds = select(fd + 1, &rfds, NULL, NULL, NULL);
        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            WARN(NULL, "error monitoring XenStore");
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

    if (tapback_log_fp) {
        fprintf(tapback_log_fp, "%s[%s] ", blkback_ident, strprio[prio]);
        vfprintf(tapback_log_fp, fmt, ap);
    }
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
            "\t[-d|--debug]\n"
			"\t[-h|--help]\n"
            "\t[-v|--verbose]\n", prog);
}

int main(int argc, char **argv)
{
    const char *prog = NULL;
    bool opt_debug = false, opt_verbose = false;
    int err = 0;

	if (access("/dev/xen/gntdev", F_OK ) == -1) {
		WARN(NULL, "grant device does not exist\n");
		err = EINVAL;
		goto fail;
	}

    prog = basename(argv[0]);

    opt_debug = 0;

    do {
        const struct option longopts[] = {
            {"help", 0, NULL, 'h'},
            {"debug", 0, NULL, 'd'},
            {"verbose", 0, NULL, 'v'},
        };
        int c;

        c = getopt_long(argc, argv, "h:dv", longopts, NULL);
        if (c < 0)
            break;

        switch (c) {
        case 'h':
            usage(stdout, prog);
            return 0;
        case 'd':
            opt_debug = true;
            break;
        case 'v':
            opt_verbose = true;
            break;
        case '?':
            goto usage;
        }
    } while (1);

    if (opt_verbose) {
        blkback_ident = "";
        tapback_log_fp = fopen(TAPBACK_LOG, "a+");
        if (!tapback_log_fp)
            return errno;
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
    if (tapback_log_fp) {
        INFO(NULL, "tapback shut down\n");
        fclose(tapback_log_fp);
    }
    return err ? -err : 0;

usage:
    usage(stderr, prog);
    return 1;
}

int pretty_time(char *buf, unsigned char buf_len) {

    int err;
    time_t timer;
    struct tm* tm_info;

    assert(buf);
    assert(buf_len > 0);

    err = time(&timer);
    if (err == (time_t)-1)
        return errno;
    tm_info = localtime(&timer);
    if (!tm_info)
        return EINVAL;
    err = strftime(buf, buf_len, "%b %d %H:%M:%S", tm_info);
    if (err == 0)
        return EINVAL;
    return 0;
}
