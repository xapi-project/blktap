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

LIST_HEAD(backends);

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
tapback_read_watch(backend_t *backend)
{
    char **watch = NULL, *path = NULL, *token = NULL;
    unsigned int n = 0;
    int err = 0;
    char *s;

	ASSERT(backend);

    /* read the change */
    watch = xs_read_watch(backend->xs, &n);
    path = watch[XS_WATCH_PATH];
    token = watch[XS_WATCH_TOKEN];

    /*
     * print the path the watch triggered on for debug purposes
     *
     * TODO include token
	 * TODO put in an #if DEBUG block
     */
	s = tapback_xs_read(backend->xs, XBT_NULL, "%s", path);
    if (s) {
        if (0 == strlen(s))
            DBG(NULL, "%s -> (created)\n", path);
        else
            DBG(NULL, "%s -> \'%s\'\n", path, s);
        free(s);
    } else {
		err = errno;
		if (err == ENOENT)
	        DBG(NULL, "%s -> (removed)\n", path);
		else
			WARN(NULL, "failed to read %s: %s\n", path, strerror(err));
	}

    /*
     * The token indicates which XenStore watch triggered, the front-end one or
     * the back-end one.
     */
    if (!strcmp(token, BLKTAP3_FRONTEND_TOKEN)) {
        err = -tapback_backend_handle_otherend_watch(backend, path);
    } else if (!strcmp(token, backend->token)) {
        err = -tapback_backend_handle_backend_watch(backend, path);
    } else {
        WARN(NULL, "invalid token \'%s\'\n", token);
        err = EINVAL;
    }

	if (err)
		WARN(NULL, "failed to process XenStore watch on %s: %s\n",
				path, strerror(err));

    free(watch);
    return;
}

static void
tapback_backend_destroy(backend_t *backend)
{
    int err;

	if (!backend)
		return;

    free(backend->name);
    free(backend->path);
    free(backend->token);

    if (backend->xs) {
        xs_daemon_close(backend->xs);
        backend->xs = NULL;
    }

    err = unlink(TAPBACK_CTL_SOCK_PATH);
    if (err == -1 && errno != ENOENT) {
        err = errno;
        WARN(NULL, "failed to remove %s: %s\n", TAPBACK_CTL_SOCK_PATH,
				strerror(err));
    }

	list_del(&backend->entry);

	free(backend);
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
		backend_t *backend, *tmp;

		list_for_each_entry(backend, &backends, entry) {
			if (!list_empty(&backend->devices)) {
				WARN(NULL, "refusing to shutdown while back-end %s has active "
						"VBDs\n", backend->name);
				return;
			}
		}

		list_for_each_entry_safe(backend, tmp, &backends, entry)
			tapback_backend_destroy(backend);

        exit(EXIT_SUCCESS);
    }
}

/**
 * Initializes the back-end descriptor. There is one back-end per tapback
 * process. Also, it initiates a watch to XenStore on backend/<backend name>.
 *
 * @returns a new back-end, NULL on failure, sets errno
 */
static backend_t *
tapback_backend_create(const char *name)
{
    int err;
    struct sockaddr_un local;
    int len;
	backend_t *backend = NULL;

    ASSERT(name);

	backend = calloc(1, sizeof(*backend));
	if (!backend) {
		err = errno;
		goto out;
	}
	INIT_LIST_HEAD(&backend->entry);
	INIT_LIST_HEAD(&backend->devices);

    backend->path = backend->token = NULL;

    backend->name = strdup(name);
    if (!backend->name) {
        err = errno;
        goto out;
    }

    err = asprintf(&backend->path, "%s/%s", XENSTORE_BACKEND,
            backend->name);
    if (err == -1) {
        backend->path = NULL;
        err = errno;
        goto out;
    }

    err = asprintf(&backend->token, "%s-%s", XENSTORE_BACKEND,
            backend->name);
    if (err == -1) {
        backend->token = NULL;
        err = errno;
        goto out;
    }

    err = 0;

    INIT_LIST_HEAD(&backend->devices);
    backend->ctrl_sock = -1;

    if (!(backend->xs = xs_daemon_open())) {
        err = EINVAL;
        goto out;
    }

	err = get_my_domid(backend->xs, XBT_NULL);
	if (err < 0) {
		/*
		 * If the domid XenStore key is not yet written, it means we're running
		 * in dom0, otherwise if we were running in a driver domain the key
		 * would have been written before the domain had even started.
		 *
		 * XXX We can always set a XenStore watch as a fullproof solution.
		 */
		if (err == -ENOENT) {
			INFO(NULL, "domid XenStore key not yet present, assuming we are "
					"domain 0\n");
			err = 0;
		} else {
			err = -err;
			WARN(NULL, "failed to get current domain ID: %s\n", strerror(err));
			goto out;
		}
	}
	backend->domid = err;
	err = 0;

    /*
     * Watch the back-end.
     */
    if (!xs_watch(backend->xs, backend->path,
                backend->token)) {
        err = errno;
        goto out;
    }

    if (SIG_ERR == signal(SIGINT, signal_cb) ||
            SIG_ERR == signal(SIGTERM, signal_cb) ||
            SIG_ERR == signal(SIGHUP, signal_cb)) {
        WARN(NULL, "failed to register signal handlers\n");
        err = EINVAL;
        goto out;
    }

    /*
     * Create the control socket.
     * XXX We don't listen for connections as we don't yet support any control
     * commands.
     */
    backend->ctrl_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (backend->ctrl_sock == -1) {
        err = errno;
        WARN(NULL, "failed to create control socket: %s\n", strerror(errno));
        goto out;
    }
    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, TAPBACK_CTL_SOCK_PATH);
    err = unlink(local.sun_path);
    if (err && errno != ENOENT) {
        err = errno;
        WARN(NULL, "failed to remove %s: %s\n", local.sun_path, strerror(err));
        goto out;
    }
    len = strlen(local.sun_path) + sizeof(local.sun_family);
    err = bind(backend->ctrl_sock, (struct sockaddr *)&local, len);
    if (err == -1) {
        err = errno;
        WARN(NULL, "failed to bind to %s: %s\n", local.sun_path, strerror(err));
        goto out;
    }

	list_add(&backend->entry, &backends);

out:
	if (err) {
		tapback_backend_destroy(backend);
		backend = NULL;
		errno = err;
	}

    return backend;
}

/**
 * Runs the daemon.
 *
 * Watches backend/<backend name> and the front-end devices.
 */
static inline int
tapback_backend_run(backend_t *backend)
{
    int fd;
	int err;

	ASSERT(backend);

	fd = xs_fileno(backend->xs);

    INFO(NULL, "tapback (user-space blkback for tapdisk3) daemon started\n");

    do {
        fd_set rfds;
        int nfds = 0;
        struct timeval ttl = {.tv_sec = 3, .tv_usec = 0};

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        /*
         * poll the fd for changes in the XenStore path we're interested in
         */
        nfds = select(fd + 1, &rfds, NULL, NULL, &ttl);
        if (nfds == 0) {
            fflush(tapback_log_fp);
            continue;
        }
        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            WARN(NULL, "error monitoring XenStore");
            err = -errno;
            break;
        }

        if (FD_ISSET(fd, &rfds))
            tapback_read_watch(backend);
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
            "\t[-v|--verbose]\n"
            "\t[-n|--name]\n", prog);
}

extern char *optarg;

int main(int argc, char **argv)
{
    const char *prog = NULL;
    char *opt_name = "vbd3";
    bool opt_debug = false, opt_verbose = false;
    int err = 0;
	backend_t *backend = NULL;

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
            {"name", 0, NULL, 'n'},
        };
        int c;

        c = getopt_long(argc, argv, "hdvn:", longopts, NULL);
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
        case 'n':
            opt_name = strdup(optarg);
            if (!opt_name) {
                err = errno;
                goto fail;
            }
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
        blkback_ident = opt_name;
        openlog(blkback_ident, 0, LOG_DAEMON);
        setlogmask(LOG_UPTO(LOG_INFO));
    }

    if (!opt_debug) {
        if ((err = daemon(0, 0))) {
            err = -errno;
            goto fail;
        }
    }

	backend = tapback_backend_create(opt_name);
	if (!backend) {
		err = errno;
        WARN(NULL, "error creating blkback: %s\n", strerror(err));
        goto fail;
    }

    err = tapback_backend_run(backend);

    tapback_backend_destroy(backend);

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

/**
 * Returns the current domain ID or -errno.
 */
int
get_my_domid(struct xs_handle * const xs, xs_transaction_t xst)
{
	char *buf = NULL, *end = NULL;
	int domid;

	buf = tapback_xs_read(xs, xst, "domid");
	if (!buf) {
		domid = -errno;
		goto out;
	}

	domid = strtoul(buf, &end, 0);
	if (*end != 0 || end == buf) {
		domid = -EINVAL;
	}
out:
	free(buf);
	if (domid >= 0)
		ASSERT(domid <= UINT16_MAX);
	return domid;
}
