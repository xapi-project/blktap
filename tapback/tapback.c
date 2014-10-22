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

#include "config.h"
#include "blktap3.h"
#include "stdio.h" /* TODO tap-ctl.h needs to include stdio.h */
#include "tap-ctl.h"
#include "tapback.h"
#include <signal.h>

const char tapback_name[] = "tapback";
unsigned log_level;

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

	ASSERT(backend);

    /* read the change */
    watch = xs_read_watch(backend->xs, &n);
    path = watch[XS_WATCH_PATH];
    token = watch[XS_WATCH_TOKEN];

    /*
     * print the path the watch triggered on for debug purposes
     *
     * TODO include token
     */
    if (verbose()) {
        char *val = tapback_xs_read(backend->xs, XBT_NULL, "%s", path);
        if (val) {
            if (0 == strlen(val))
                /*
                 * XXX "(created)" might be printed when the a XenStore
                 * directory gets removed, the XenStore watch fires, and a
                 * XenStore node is created under the directory that just got
                 * removed. This usually happens when the toolstack removes the
                 * VBD from XenStore and then immediately writes the
                 * tools/xenops/cancel key in it.
                 */
                DBG(NULL, "%s -> (created)\n", path);
            else
                DBG(NULL, "%s -> \'%s\'\n", path, val);
            free(val);
        } else {
            err = errno;
            if (err == ENOENT)
                DBG(NULL, "%s -> (removed)\n", path);
            else
                WARN(NULL, "failed to read %s: %s\n", path, strerror(err));
        }
    }

    /*
     * The token indicates which XenStore watch triggered, the front-end one or
     * the back-end one.
     */
    if (!strcmp(token, backend->frontend_token)) {
        ASSERT(!tapback_is_master(backend));
        err = -tapback_backend_handle_otherend_watch(backend, path);
    } else if (!strcmp(token, backend->backend_token)) {
        err = -tapback_backend_handle_backend_watch(backend, path);
    } else {
        WARN(NULL, "invalid token \'%s\'\n", token);
        err = EINVAL;
    }

	if (err)
		WARN(NULL, "failed to process XenStore watch on %s: %s\n",
				path, strerror(abs(err)));

    free(watch);
    return;
}

/**
 * NB must be async-signal-safe.
 */
void
tapback_backend_destroy(backend_t *backend)
{
	if (!backend)
		return;

    if (backend->pidfile) {
        unlink(backend->pidfile);
        free(backend->pidfile);
    }

    free(backend->name);
    free(backend->path);
    free(backend->frontend_token);
    free(backend->backend_token);

    if (backend->xs) {
        xs_daemon_close(backend->xs);
        backend->xs = NULL;
    }

    unlink(backend->local.sun_path);

	list_del(&backend->entry);

	free(backend);
}

static void
tapback_signal_handler(int signum, siginfo_t *siginfo __attribute__((unused)),
        void *context __attribute__((unused)))
{
    if (likely(signum == SIGINT || signum == SIGTERM)) {
		backend_t *backend, *tmp;

		list_for_each_entry(backend, &backends, entry) {
            if (tapback_is_master(backend)) {
                if (backend->master.slaves)
                    return;
            } else {
                if (!list_empty(&backend->slave.slave.devices))
                    return;
            }
		}

		list_for_each_entry_safe(backend, tmp, &backends, entry)
			tapback_backend_destroy(backend);

        _exit(EXIT_SUCCESS);
    }
}

static inline int
tapback_write_pid(const char *pidfile)
{
    FILE *fp;
    int err = 0;

    ASSERT(pidfile);

    fp = fopen(pidfile, "w");
    if (!fp)
        return errno;
    err = fprintf(fp, "%d\n", getpid());
    if (err < 0)
        err = errno;
    else
        err = 0;
    fclose(fp);
    return err;
}

/**
 * Initializes the back-end descriptor. There is one back-end per tapback
 * process. Also, it initiates a watch to XenStore on backend/<backend name>.
 *
 * @returns a new back-end, NULL on failure, sets errno
 */
static inline backend_t *
tapback_backend_create(const char *name, const char *pidfile,
        const domid_t domid, const bool barrier)
{
    int err;
    int len;
	backend_t *backend = NULL;

    ASSERT(name);

	backend = calloc(1, sizeof(*backend));
	if (!backend) {
		err = errno;
		goto out;
	}

    if (pidfile) {
        backend->pidfile = strdup(pidfile);
        if (unlikely(!backend->pidfile)) {
            err = errno;
            WARN(NULL, "failed to strdup: %s\n", strerror(err));
            goto out;
        }
        err = tapback_write_pid(backend->pidfile);
        if (unlikely(err)) {
            WARN(NULL, "failed to write PID to %s: %s\n",
                    pidfile, strerror(err));
            goto out;
        }
    }

    backend->name = strdup(name);
    if (!backend->name) {
        err = errno;
        goto out;
    }

	backend->barrier = barrier;

    backend->path = NULL;

    INIT_LIST_HEAD(&backend->entry);

    if (domid) {
        backend->slave_domid = domid;
        INIT_LIST_HEAD(&backend->slave.slave.devices);
        err = asprintf(&backend->path, "%s/%s/%d", XENSTORE_BACKEND,
                backend->name, backend->slave_domid);
        if (err == -1) {
            backend->path = NULL;
            err = errno;
            goto out;
        }
    } else {
        backend->master.slaves = NULL;
        err = asprintf(&backend->path, "%s/%s", XENSTORE_BACKEND,
                backend->name);
        if (err == -1) {
            backend->path = NULL;
            err = errno;
            goto out;
        }
    }

    if (domid) {
        err = asprintf(&backend->frontend_token, "%s-%d-front", tapback_name,
                domid);
        if (err == -1) {
            backend->frontend_token = NULL;
            err = errno;
            goto out;
        }

        err = asprintf(&backend->backend_token, "%s-%d-back", tapback_name,
                domid);
        if (err == -1) {
            backend->backend_token = NULL;
            err = errno;
            goto out;
        }
    } else {
        err = asprintf(&backend->frontend_token, "%s-master-front",
                tapback_name);
        if (err == -1) {
            backend->frontend_token = NULL;
            err = errno;
            goto out;
        }

        err = asprintf(&backend->backend_token, "%s-master-back",
                tapback_name);
        if (err == -1) {
            backend->backend_token = NULL;
            err = errno;
            goto out;
        }
    }

    err = 0;

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
    if (!xs_watch(backend->xs, backend->path, backend->backend_token)) {
        err = errno;
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
    backend->local.sun_family = AF_UNIX;
    if (domid)
        err = snprintf(backend->local.sun_path,
                ARRAY_SIZE(backend->local.sun_path),
                "/var/run/%s.%d", tapback_name, domid);
    else
        err = snprintf(backend->local.sun_path,
                ARRAY_SIZE(backend->local.sun_path),
                "/var/run/%s.master", tapback_name);
    if (err >= (int)ARRAY_SIZE(backend->local.sun_path)) {
        err = ENAMETOOLONG;
        WARN(NULL, "UNIX domain socket name too long\n");
        goto out;
    } else if (err < 0) {
        err = errno;
        WARN(NULL, "failed to snprintf: %s\n", strerror(err));
        goto out;
    }
    err = 0;

    err = unlink(backend->local.sun_path);
    if (err && errno != ENOENT) {
        err = errno;
        WARN(NULL, "failed to remove %s: %s\n", backend->local.sun_path,
                strerror(err));
        goto out;
    }
    len = strlen(backend->local.sun_path) + sizeof(backend->local.sun_family);
    err = bind(backend->ctrl_sock, (struct sockaddr *)&backend->local, len);
    if (err == -1) {
        err = errno;
        WARN(NULL, "failed to bind to %s: %s\n", backend->local.sun_path,
                strerror(err));
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

    if (tapback_is_master(backend))
        INFO(NULL, "master tapback daemon started\n");
    else
        INFO(NULL, "slave tapback daemon started, only serving domain %d\n",
                backend->slave_domid);

    do {
        fd_set rfds;
        int nfds = 0;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        /*
         * poll the fd for changes in the XenStore path we're interested in
         */
        nfds = select(fd + 1, &rfds, NULL, NULL, NULL);
        if (nfds == -1) {
            if (likely(errno == EINTR))
                continue;
            err = -errno;
            WARN(NULL, "error monitoring XenStore: %s\n", strerror(-err));
            break;
        }

        if (FD_ISSET(fd, &rfds))
            tapback_read_watch(backend);
        DBG(NULL, "--\n");
    } while (1);

    return err;
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
			"\t[-b]--nobarrier]\n"
            "\t[-n|--name]\n", prog);
}

extern char *optarg;

/**
 * Returns 0 in success, -errno on failure.
 */
static inline int
tapback_install_sighdl(void)
{
    int err;
    struct sigaction sigact;
	sigset_t set;
    static const int signals[] = {SIGTERM, SIGINT, SIGCHLD};
    unsigned i;

    err = sigfillset(&set);
    if (unlikely(err == -1)) {
        err = errno;
        WARN(NULL, "failed to fill signal set: %s\n", strerror(err));
        goto out;
    }

    for (i = 0; i < ARRAY_SIZE(signals); i++) {
        err = sigdelset(&set, signals[i]);
        if (unlikely(err == -1)) {
            err = errno;
            WARN(NULL, "failed to add signal %d to signal set: %s\n",
                    signals[i], strerror(err));
            goto out;
        }
    }

    err = sigprocmask(SIG_BLOCK, &set, NULL);
	if (unlikely(err == -1)) {
        err = errno;
        WARN(NULL, "failed to set signal mask: %s\n", strerror(err));
        goto out;
	}

    sigact.sa_sigaction = &tapback_signal_handler;
    sigact.sa_flags = SA_SIGINFO | SA_NOCLDSTOP | SA_NOCLDWAIT;

    for (i = 0; i < ARRAY_SIZE(signals); i++) {
        err = sigaction(signals[i], &sigact, NULL);
        if (unlikely(err == -1)) {
            err = errno;
            WARN(NULL, "failed to register signal %d: %s\n",
                    signals[i], strerror(err));
            goto out;
        }
    }
out:
    return -err;
}

int main(int argc, char **argv)
{
    const char *prog = NULL;
    char *opt_name = "vbd3", *opt_pidfile = NULL, *end = NULL;
    bool opt_debug = false, opt_verbose = false;
    int err = 0;
	backend_t *backend = NULL;
    domid_t opt_domid = 0;
	bool opt_barrier = true;

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
            {"pidfile", 0, NULL, 'p'},
            {"domain", 0, NULL, 'x'},
			{"nobarrier", 0, NULL, 'b'},

        };
        int c;

        c = getopt_long(argc, argv, "hdvn:p:x:b", longopts, NULL);
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
        case 'p':
            opt_pidfile = strdup(optarg);
            if (!opt_pidfile) {
                err = errno;
                goto fail;
            }
            break;
        case 'x':
            opt_domid = strtoul(optarg, &end, 0);
            if (*end != 0 || end == optarg) {
                WARN(NULL, "invalid domain ID %s\n", optarg);
                err = EINVAL;
                goto fail;
            }
            INFO(NULL, "only serving domain %d\n", opt_domid);
            break;
		case 'b':
			opt_barrier = false;
			break;
        case '?':
            goto usage;
        }
    } while (1);

    if (!opt_debug) {
        if ((err = daemon(0, 0))) {
            err = -errno;
            goto fail;
        }
    }

    openlog(tapback_name, LOG_PID, LOG_DAEMON);
    if (opt_verbose)
        log_level = LOG_DEBUG;
    else
        log_level = LOG_INFO;
    setlogmask(LOG_UPTO(log_level));

    if (!opt_debug) {
        if ((err = daemon(0, 0))) {
            err = -errno;
            goto fail;
        }
    }

    err = tapback_install_sighdl();
    if (unlikely(err))
    {
        WARN(NULL, "failed to set up signal handling: %s\n", strerror(-err));
        goto fail;
    }

	backend = tapback_backend_create(opt_name, opt_pidfile, opt_domid,
			opt_barrier);
	if (!backend) {
		err = errno;
        WARN(NULL, "error creating back-end: %s\n", strerror(err));
        goto fail;
    }

    err = tapback_backend_run(backend);

    tapback_backend_destroy(backend);

fail:
    return err ? -err : 0;

usage:
    usage(stderr, prog);
    return 1;
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

bool
tapback_is_master(const backend_t *backend)
{
    if (backend->slave_domid == 0)
        return true;
    else
        return false;
}

int
compare(const void *pa, const void *pb)
{
    const struct backend_slave *_pa, *_pb;

    ASSERT(pa);
    ASSERT(pb);

    _pa = pa;
    _pb = pb;

    return _pa->master.domid - _pb->master.domid;
}
