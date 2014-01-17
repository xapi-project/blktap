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

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include "block-valve.h"
#include "compiler.h"
#include "list.h"
#include "util.h"

static void
rlb_vlog_vfprintf(int prio, const char *fmt, va_list ap)
{
	vfprintf(stderr, fmt, ap); fputc('\n', stderr);
}

static void (*rlb_vlog)(int prio, const char *fmt, va_list ap);

__printf(2, 3)
static void
rlb_log(int prio, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt); rlb_vlog(prio, fmt, ap); va_end(ap);
}

static int debug = 0;

#define DBG(_l, _f, _a...) if (debug >= _l) { rlb_log(LOG_DEBUG, _f, ##_a); }
#define INFO(_f, _a...)    rlb_log(LOG_INFO, _f, ##_a)
#define WARN(_f, _a...)    rlb_log(LOG_WARNING, "WARNING: " _f ", in %s:%d", \
				   ##_a, __func__, __LINE__)
#define ERR(_f, _a...)     rlb_log(LOG_ERR, "ERROR: " _f ", in %s:%d", \
				   ##_a, __func__, __LINE__)
#define PERROR(_f, _a...)  rlb_log(LOG_ERR, _f ": %s in %s:%d", \
				   ##_a, strerror(errno), __func__, __LINE__)

#define BUG() do {						\
		ERR("Aborting");				\
		abort();					\
	} while (0)

#define BUG_ON(_cond)						\
	if (unlikely(_cond)) {					\
		ERR("(%s) = %d", #_cond, _cond);		\
		BUG();						\
	}

#define WARN_ON(_cond) ({					\
	int __cond = _cond;					\
	if (unlikely(__cond))					\
		WARN("(%s) = %d", #_cond, _cond);		\
	__cond;						\
})

#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))

typedef struct ratelimit_bridge        td_rlb_t;
typedef struct ratelimit_connection    td_rlb_conn_t;

struct ratelimit_connection {
	int                            sock;

	unsigned long                  need; /* I/O requested */
	unsigned long                  gntd; /* I/O granted, pending */

	struct list_head               open; /* connected */
	struct list_head               wait; /* need > 0 */

	struct {
		struct timeval         since;
		struct timeval         total;
	} wstat;
};

#define RLB_CONN_MAX                   1024

struct ratelimit_ops {
	void    (*usage)(td_rlb_t *rlb, FILE *stream, void *data);

	int     (*create)(td_rlb_t *rlb, int argc, char **argv, void **data);
	void    (*destroy)(td_rlb_t *rlb, void *data);

	void    (*info)(td_rlb_t *rlb, void *data);

	void    (*settimeo)(td_rlb_t *rlb, struct timeval **tv, void *data);
	void    (*timeout)(td_rlb_t *rlb, void *data);
	void    (*dispatch)(td_rlb_t *rlb, void *data);
	void    (*reset)(td_rlb_t *rlb, void *data);
};

struct ratelimit_bridge {
	char                          *name;
	char                          *ident;

	struct sockaddr_un             addr;
	char                          *path;
	int                            sock;

	struct list_head               open; /* all connections */
	struct list_head               wait; /* all in need */

	struct timeval                 ts, now;

	td_rlb_conn_t                  connv[RLB_CONN_MAX];
	td_rlb_conn_t                 *free[RLB_CONN_MAX];
	int                            n_free;

	struct rlb_valve {
		struct ratelimit_ops  *ops;
		void                  *data;
	} valve;
};

#define rlb_for_each_conn(_conn, _rlb)					\
	list_for_each_entry(_conn, &(_rlb)->open, open)

#define rlb_for_each_conn_safe(_conn, _next, _rlb)			\
	list_for_each_entry_safe(_conn, _next, &(_rlb)->open, open)

#define rlb_for_each_waiting(_conn, _next, _rlb)			\
	list_for_each_entry(_conn, _next, &(_rlb)->wait, wait)

#define rlb_for_each_waiting_safe(_conn, _next, _rlb)			\
	list_for_each_entry_safe(_conn, _next, &(_rlb)->wait, wait)

#define rlb_conn_entry(_list)			\
	list_entry(_list, td_rlb_conn_t, open)

#define rlb_wait_entry(_list)			\
	list_entry(_list, td_rlb_conn_t, wait)

static struct ratelimit_ops *rlb_find_valve(const char *name);

static int rlb_create_valve(td_rlb_t *, struct rlb_valve *,
			    const char *name, int argc, char **argv);

/*
 * util
 */

#define case_G case 'G': case 'g'
#define case_M case 'M': case 'm'
#define case_K case 'K': case 'k'

static long
rlb_strtol(const char *s)
{
	unsigned long l, u = 1;
	char *end, p, q;

	l = strtoul(s, &end, 0);
	if (!*end)
		return l;

	p = *end++;

	switch (p) {
	case_G: case_M: case_K:

		q = *end++;

		switch (q) {
		case 'i':
			switch (p) {
			case_G:
				u *= 1024;
			case_M:
				u *= 1024;
			case_K:
				u *= 1024;
			}
			break;

		case 0:
			switch (p) {
			case_G:
				u *= 1000;
			case_M:
				u *= 1000;
			case_K:
				u *= 1000;
			}
			break;

		default:
			goto fail;
		}
		break;

	case 0:
		break;

	default:
		goto fail;
	}

	return l * u;

fail:
	return -EINVAL;
}

static char*
vmprintf(const char *fmt, va_list ap)
{
	char *s;
	int n;

	n = vasprintf(&s, fmt, ap);
	if (n < 0)
		s = NULL;

	return s;
}

__printf(1, 2)
static char*
mprintf(const char *fmt, ...)
{
	va_list ap;
	char *s;

	va_start(ap, fmt);
	s = vmprintf(fmt, ap);
	va_end(ap);

	return s;
}

static int
sysctl_vscanf(const char *name, const char *fmt, va_list ap)
{
	char *path = NULL;
	FILE *s = NULL;
	int rv;

	path = mprintf("/proc/sys/%s", name);
	if (!path) {
		rv = -errno;
		goto fail;
	}

	s = fopen(path, "r");
	if (!s) {
		rv = -errno;
		goto fail;
	}

	rv = vfscanf(s, fmt, ap);
fail:
	if (s)
		fclose(s);

	if (path)
		free(path);

	return rv;
}

static int
sysctl_scanf(const char *name, const char *fmt, ...)
{
	va_list(ap);
	int rv;

	va_start(ap, fmt);
	rv = sysctl_vscanf(name, fmt, ap);
	va_end(ap);

	return rv;
}

static long
sysctl_strtoul(const char *name)
{
	unsigned val;
	int n;

	n = sysctl_scanf(name, "%lu", &val);
	if (n < 0)
		return n;
	if (n != 1)
		return -EINVAL;

	return val;
}


static long long
rlb_tv_usec(const struct timeval *tv)
{
	long long us;

	us  = tv->tv_sec;
	us *= 1000000;
	us += tv->tv_usec;

	return us;
}

static long long
rlb_usec_since(td_rlb_t *rlb, const struct timeval *since)
{
	struct timeval delta;

	timersub(&rlb->now, since, &delta);

	return rlb_tv_usec(&delta);
}

static inline void
rlb_argv_shift(int *optind, int *argc, char ***argv)
{
	/* reset optind and args after '--' */

	*optind -= 1;

	*argc   -= *optind;
	*argv   += *optind;

	*optind  = 1;
}

/*
 * socket I/O
 */

static void
rlb_sock_close(td_rlb_t *rlb)
{
	if (rlb->path) {
		unlink(rlb->path);
		rlb->path = NULL;
	}

	if (rlb->sock >= 0) {
		close(rlb->sock);
		rlb->sock = -1;
	}
}

static int
rlb_sock_open(td_rlb_t *rlb)
{
	int s, err;

	rlb->sock = -1;

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		PERROR("socket");
		err = -errno;
		goto fail;
	}

	rlb->sock = s;

	rlb->addr.sun_family = AF_UNIX;

	if (rlb->name[0] == '/')
		strncpy(rlb->addr.sun_path, rlb->name,
			sizeof(rlb->addr.sun_path));
	else
		snprintf(rlb->addr.sun_path, sizeof(rlb->addr.sun_path),
			 "%s/%s", TD_VALVE_SOCKDIR, rlb->name);

	err = bind(rlb->sock, &rlb->addr, sizeof(rlb->addr));
	if (err) {
		PERROR("%s", rlb->addr.sun_path);
		err = -errno;
		goto fail;
	}

	rlb->path = rlb->addr.sun_path;

	err = listen(rlb->sock, RLB_CONN_MAX);
	if (err) {
		PERROR("listen(%s)", rlb->addr.sun_path);
		err = -errno;
		goto fail;
	}

	return 0;

fail:
	rlb_sock_close(rlb);
	return err;
}

static int
rlb_sock_send(td_rlb_t *rlb, td_rlb_conn_t *conn,
	      const void *msg, size_t size)
{
	ssize_t n;

	n = send(conn->sock, msg, size, MSG_DONTWAIT);
	if (n < 0)
		return -errno;
	if (n && n != size)
		return -EPROTO;

	return 0;
}

static int
rlb_sock_recv(td_rlb_t *rlb, td_rlb_conn_t *conn,
	      void *msg, size_t size)
{
	ssize_t n;

	n = recv(conn->sock, msg, size, MSG_DONTWAIT);
	if (n < 0)
		return -errno;

	return n;
}

static td_rlb_conn_t *
rlb_conn_alloc(td_rlb_t *rlb)
{
	td_rlb_conn_t *conn = NULL;

	if (likely(rlb->n_free > 0))
		conn = rlb->free[--rlb->n_free];

	return conn;
}

static void
rlb_conn_free(td_rlb_t *rlb, td_rlb_conn_t *conn)
{
	BUG_ON(rlb->n_free >= RLB_CONN_MAX);

	rlb->free[rlb->n_free++] = conn;
}

static int
rlb_conn_id(td_rlb_t *rlb, td_rlb_conn_t *conn)
{
	return conn - rlb->connv;
}

static void
rlb_conn_info(td_rlb_t *rlb, td_rlb_conn_t *conn)
{
	long long wtime;
	int waits;

	wtime = 0;
	waits = !list_empty(&conn->wait);
	if (waits)
		wtime = rlb_usec_since(rlb, &conn->wstat.since) / 1000;

	WARN_ON(!!conn->need != waits);

	INFO("conn[%d] needs %lu (since %llu ms, total %lu.%06lu s),"
	     " %lu granted",
	     rlb_conn_id(rlb, conn), conn->need, wtime,
	     conn->wstat.total.tv_sec, conn->wstat.total.tv_usec,
	     conn->gntd);
}

static void
rlb_conn_infos(td_rlb_t *rlb)
{
	td_rlb_conn_t *conn;

	rlb_for_each_conn(conn, rlb)
		rlb_conn_info(rlb, conn);
}

static void
rlb_conn_close(td_rlb_t *rlb, td_rlb_conn_t *conn)
{
	int s = conn->sock;

	INFO("Connection %d closed.", rlb_conn_id(rlb, conn));
	rlb_conn_info(rlb, conn);

	if (s) {
		close(s);
		conn->sock = -1;
	}

	list_del_init(&conn->wait);
	list_del(&conn->open);

	rlb_conn_free(rlb, conn);
}

static void
rlb_conn_receive(td_rlb_t *rlb, td_rlb_conn_t *conn)
{
	struct td_valve_req buf[32], req = { -1, -1 };
	ssize_t n;
	int i, err;

	n = rlb_sock_recv(rlb, conn, buf, sizeof(buf));
	if (!n)
		goto close;

	if (n < 0) {
		err = n;
		if (err != -EAGAIN)
			goto fail;
	}

	if (unlikely(n % sizeof(req))) {
		err = -EPROTO;
		goto fail;
	}

	for (i = 0; i < n / sizeof(buf[0]); i++) {
		req = buf[i];

		if (unlikely(req.need > TD_RLB_REQUEST_MAX)) {
			err = -EINVAL;
			goto fail;
		}

		if (unlikely(req.done > conn->gntd)) {
			err = -EINVAL;
			goto fail;
		}

		conn->need += req.need;
		conn->gntd -= req.done;

		DBG(8, "rcv: %lu/%lu need=%lu gntd=%lu",
		    req.need, req.done, conn->need, conn->gntd);

		if (unlikely(conn->need > TD_RLB_REQUEST_MAX)) {
			err = -EINVAL;
			goto fail;
		}
	}

	if (conn->need && list_empty(&conn->wait)) {
		list_add_tail(&conn->wait, &rlb->wait);
		conn->wstat.since = rlb->now;
	}

	return;

fail:
	WARN("err = %d (%s)"
	     " (need %ld/%ld, %ld/%ld done),"
	     " closing connection.",
	     err, strerror(-err),
	     req.need, conn->need, req.done, conn->gntd);

	rlb_conn_info(rlb, conn);
close:
	rlb_conn_close(rlb, conn);
}

static void
rlb_conn_respond(td_rlb_t *rlb, td_rlb_conn_t *conn, unsigned long need)
{
	int err;

	BUG_ON(need > conn->need);

	err = rlb_sock_send(rlb, conn, &need, sizeof(need));
	if (err)
		goto fail;

	conn->need -= need;
	conn->gntd += need;

	DBG(8, "snd: %lu need=%lu gntd=%lu", need, conn->need, conn->gntd);

	if (!conn->need) {
		struct timeval delta;

		timersub(&rlb->now, &conn->wstat.since, &delta);
		timeradd(&conn->wstat.total, &delta, &conn->wstat.total);

		list_del_init(&conn->wait);
	}

	return;

fail:
	WARN("err = %d, killing connection.", err);
	rlb_conn_close(rlb, conn);
}

static void
rlb_accept_conn(td_rlb_t *rlb)
{
	td_rlb_conn_t *conn;
	int s, err;

	s = accept(rlb->sock, NULL, NULL);
	if (!s) {
		err = -errno;
		goto fail;
	}

	conn = rlb_conn_alloc(rlb);
	if (!conn) {
		err = -ENOMEM;
		close(s);
		goto fail;
	}

	INFO("Accepting connection %td.", conn - rlb->connv);

	memset(conn, 0, sizeof(*conn));
	INIT_LIST_HEAD(&conn->wait);
	conn->sock = s;
	list_add_tail(&conn->open, &rlb->open);

	return;

fail:
	WARN("err = %d", err);
}

static long long
rlb_pending(td_rlb_t *rlb)
{
	td_rlb_conn_t *conn;
	long long pend = 0;

	rlb_for_each_conn(conn, rlb)
		pend += conn->gntd;

	return pend;
}

/*
 * token bucket valve
 */

typedef struct ratelimit_token td_rlb_token_t;

struct ratelimit_token {
	long                      cred;
	long                      cap;
	long                      rate;
	struct timeval            timeo;
};

static void
rlb_token_settimeo(td_rlb_t *rlb, struct timeval **_tv, void *data)
{
	td_rlb_token_t *token = data;
	struct timeval *tv = &token->timeo;
	long long us;

	if (list_empty(&rlb->wait)) {
		*_tv = NULL;
		return;
	}

	WARN_ON(token->cred >= 0);

	us  = -token->cred;
	us *= 1000000;
	us /= token->rate;

	tv->tv_sec  = us / 1000000;
	tv->tv_usec = us % 1000000;

	WARN_ON(!timerisset(tv));

	*_tv = tv;
}

static void
rlb_token_refill(td_rlb_t *rlb, td_rlb_token_t *token)
{
	struct timeval tv;
	long long cred, max_usec;

	/* max time needed to refill up to cap */

	max_usec  = token->cap - token->cred;
	max_usec *= 1000000;
	max_usec += token->rate - 1;
	max_usec /= token->rate;

	/* actual credit gained */

	timersub(&rlb->now, &rlb->ts, &tv);

	cred  = rlb_tv_usec(&tv);
	cred  = MIN(cred, max_usec);
	cred *= token->rate;
	cred /= 1000000;

	/* up to cap */

	token->cred += cred;
	token->cred  = MIN(token->cred, token->cap);
}

static void
rlb_token_dispatch(td_rlb_t *rlb, void *data)
{
	td_rlb_token_t *token = data;
	td_rlb_conn_t *conn, *next;

	rlb_token_refill(rlb, token);

	rlb_for_each_waiting_safe(conn, next, rlb) {
		if (token->cred < 0)
			break;

		token->cred -= conn->need;

		rlb_conn_respond(rlb, conn, conn->need);
	}
}

static void
rlb_token_reset(td_rlb_t *rlb, void *data)
{
	td_rlb_token_t *token = data;

	token->cred = token->cap;
}

static void
rlb_token_destroy(td_rlb_t *rlb, void *data)
{
	td_rlb_token_t *token = data;

	if (token)
		free(token);
}

static int
rlb_token_create(td_rlb_t *rlb, int argc, char **argv, void **data)
{
	td_rlb_token_t *token;
	int err;

	token = calloc(1, sizeof(*token));
	if (!token) {
		err = -ENOMEM;
		goto fail;
	}

	token->rate = 0;
	token->cap  = 0;

	do {
		const struct option longopts[] = {
			{ "rate",        1, NULL, 'r' },
			{ "cap",         1, NULL, 'c' },
			{ NULL,          0, NULL,  0  }
		};
		int c;

		c = getopt_long(argc, argv, "r:c:", longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'r':
			token->rate = rlb_strtol(optarg);
			if (token->rate < 0) {
				ERR("invalid --rate");
				goto usage;
			}
			break;

		case 'c':
			token->cap = rlb_strtol(optarg);
			if (token->cap < 0) {
				ERR("invalid --cap");
				goto usage;
			}
			break;

		case '?':
			goto usage;

		default:
			BUG();
		}
	} while (1);

	if (!token->rate) {
		ERR("--rate required");
		goto usage;
	}

	rlb_token_reset(rlb, token);

	*data = token;

	return 0;

fail:
	if (token)
		free(token);

	return err;

usage:
	err = -EINVAL;
	goto fail;
}

static void
rlb_token_usage(td_rlb_t *rlb, FILE *stream, void *data)
{
	fprintf(stream,
		" {-t|--type}=token --"
		" {-r|--rate}=<rate [KMG]>"
		" {-c|--cap}=<size [KMG]>");
}

static void
rlb_token_info(td_rlb_t *rlb, void *data)
{
	td_rlb_token_t *token = data;

	INFO("TOKEN: rate: %ld B/s cap: %ld B cred: %ld B",
	     token->rate, token->cap, token->cred);
}

static struct ratelimit_ops rlb_token_ops = {
	.usage    = rlb_token_usage,
	.create   = rlb_token_create,
	.destroy  = rlb_token_destroy,
	.info     = rlb_token_info,

	.settimeo = rlb_token_settimeo,
	.timeout  = rlb_token_dispatch,
	.dispatch = rlb_token_dispatch,
	.reset    = rlb_token_reset,
};

/*
 * meminfo valve
 */

typedef struct ratelimit_meminfo td_rlb_meminfo_t;

struct ratelimit_meminfo {
	unsigned int                   period;
	struct timeval                 ts;

	FILE                          *s;

	unsigned long                  total;
	unsigned long                  dirty;
	unsigned long                  writeback;

	unsigned int                   limit_hi;
	unsigned int                   limit_lo;
	unsigned int                   congested;

	struct rlb_valve               valve;
	struct timeval                 timeo;
};

static void
rlb_meminfo_info(td_rlb_t *rlb, void *data)
{
	td_rlb_meminfo_t *m = data;

	INFO("MEMINFO: lo/hi: %u/%u%% period: %u ms",
	     m->limit_lo, m->limit_hi, m->period);

	INFO("MEMINFO: total %lu kB, dirty/writeback %lu/%lu kB",
	     m->total, m->dirty, m->writeback);

	m->valve.ops->info(rlb, m->valve.data);
}

static void
rlb_meminfo_close(td_rlb_meminfo_t *m)
{
	if (m->s) {
		fclose(m->s);
		m->s = NULL;
	}
}

static int
rlb_meminfo_open(td_rlb_meminfo_t *m)
{
	FILE *s;
	int err;

	m->s = NULL;

	s = fopen("/proc/meminfo", "r");
	if (!s) {
		err = -errno;
		goto fail;
	}

	m->s = s;

	return 0;

fail:
	rlb_meminfo_close(m);
	return err;
}

static inline int __test_bit(int n, unsigned long *bitmap)
{
	return !!(*bitmap & (1UL<<n));
}

static inline void __clear_bit(int n, unsigned long *bitmap)
{
	*bitmap &= ~(1UL<<n);
}

static struct ratelimit_meminfo_scan {
	const char    *format;
	ptrdiff_t      ptrdiff;
} rlb_meminfo_scanfs[] = {
	{ "MemTotal:  %lu kB",
	  offsetof(struct ratelimit_meminfo, total) },
	{ "Dirty:     %lu kB",
	  offsetof(struct ratelimit_meminfo, dirty) },
	{ "Writeback: %lu kB",
	  offsetof(struct ratelimit_meminfo, writeback) },
};

static int
rlb_meminfo_scan(td_rlb_meminfo_t *m)
{
	const int n_keys = ARRAY_SIZE(rlb_meminfo_scanfs);
	unsigned long pending;
	int err;

	err = rlb_meminfo_open(m);
	if (err)
		goto fail;

	pending = (1UL << n_keys) - 1;

	do {
		char buf[80], *b;
		int i;

		b = fgets(buf, sizeof(buf), m->s);
		if (!b)
			break;

		for (i = 0; i < n_keys; i++) {
			struct ratelimit_meminfo_scan *scan;
			unsigned long val, *ptr;
			int n;

			if (!__test_bit(i, &pending))
				continue;

			scan = &rlb_meminfo_scanfs[i];

			n = sscanf(buf, scan->format, &val);
			if (n != 1)
				continue;

			ptr  = (void*)m + scan->ptrdiff;
			*ptr = val;

			__clear_bit(i, &pending);
		}

	} while (pending);

	if (pending) {
		err = -ESRCH;
		goto fail;
	}

	err = 0;
fail:
	rlb_meminfo_close(m);
	return err;
}

static void
rlb_meminfo_usage(td_rlb_t *rlb, FILE *stream, void *data)
{
	td_rlb_meminfo_t *m = data;

	fprintf(stream,
		" {-t|--type}=meminfo "
		" {-H|--high}=<percent> {-L|--low}=<percent>"
		" {-p|--period}=<msecs> --");

	if (m && m->valve.ops) {
		m->valve.ops->usage(rlb, stream, m->valve.data);
	} else
		fprintf(stream, " {-t|--type}={...}");
}

static void
rlb_meminfo_destroy(td_rlb_t *rlb, void *data)
{
	td_rlb_meminfo_t *m = data;

	if (m) {
		if (m->valve.data) {
			m->valve.ops->destroy(rlb, m->valve.data);
			m->valve.data = NULL;
		}

		free(m);
	}
}

static int
rlb_meminfo_create(td_rlb_t *rlb, int argc, char **argv, void **data)
{
	td_rlb_meminfo_t *m;
	const char *type;
	long dbr;
	int err;

	m = calloc(1, sizeof(*m));
	if (!m) {
		PERROR("calloc");
		err = -errno;
		goto fail;
	}

	type      = NULL;
	m->period = 100;

	do {
		const struct option longopts[] = {
			{ "period",    1, NULL, 'p' },
			{ "type",      1, NULL, 't' },
			{ "high",      1, NULL, 'H' },
			{ "low",       1, NULL, 'L' },
			{ NULL,        0, NULL,  0  }
		};
		int c;

		c = getopt_long(argc, argv, "p:t:H:L:", longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'p':
			m->period = rlb_strtol(optarg);
			if (m->period < 0)
				goto usage;
			break;

		case 'H':
			m->limit_hi = strtoul(optarg, NULL, 0);
			break;

		case 'L':
			m->limit_lo = strtoul(optarg, NULL, 0);
			break;

		case 't':
			type = optarg;
			break;

		case '?':
			goto usage;

		default:
			BUG();
		}
	} while (1);

	if (!m->limit_hi || !m->limit_lo) {
		ERR("--high/--low required");
		goto usage;
	}

	if (m->limit_lo >= m->limit_hi) {
		ERR("invalid --high/--low ratio");
		goto usage;
	}

	if (!type) {
		ERR("(sub) --type required");
		goto usage;
	}

	dbr = sysctl_strtoul("vm/dirty_background_ratio");
	if (dbr < 0) {
		err = dbr;
		ERR("vm/dirty_background_ratio: %d", err);
		goto fail;
	}

	if (0 && m->limit_lo < dbr) {
		ERR("--low %u is less than vm.dirty_background_ratio (= %ld)",
		    m->limit_lo, dbr);
		err = -EINVAL;
		goto fail;
	}

	*data = m;

	rlb_argv_shift(&optind, &argc, &argv);

	err = rlb_create_valve(rlb, &m->valve, type, argc, argv);
	if (err) {
		if (err == -EINVAL)
			goto usage;
		goto fail;
	}

	err = rlb_meminfo_scan(m);
	if (err) {
		PERROR("/proc/meminfo");
		goto fail;
	}

	return 0;

fail:
	ERR("err = %d", err);
	return err;

usage:
	err = -EINVAL;
	return err;
};

static void
rlb_meminfo_settimeo(td_rlb_t *rlb, struct timeval **_tv, void *data)
{
	td_rlb_meminfo_t *m = data;
	int idle;

	idle = list_empty(&rlb->wait);
	BUG_ON(!idle && !m->congested);

	if (m->congested) {
		m->valve.ops->settimeo(rlb, _tv, m->valve.data);
		return;
	}

	*_tv = NULL;
}

static void
rlb_meminfo_timeout(td_rlb_t *rlb, void *data)
{
	td_rlb_meminfo_t *m = data;

	WARN_ON(!m->congested);

	if (m->congested)
		m->valve.ops->timeout(rlb, m->valve.data);
}

static int
rlb_meminfo_test_high(td_rlb_t *rlb, td_rlb_meminfo_t *m, long long cred)
{
	long long lo;

	if (m->congested) {
		/* hysteresis */

		lo  = m->total;
		lo *= m->limit_lo;
		lo /= 100;

		if (cred >= lo)
			return 0;

	} else
		if (cred <= 0) {
			m->valve.ops->reset(rlb, m->valve.data);
			return 1;
		}

	return m->congested;
}

static void
rlb_meminfo_dispatch_low(td_rlb_t *rlb, td_rlb_meminfo_t *m,
			 long long *_cred)
{
	td_rlb_conn_t *conn, *next;
	long long cred = *_cred, grant;

	rlb_for_each_waiting_safe(conn, next, rlb) {

		if (cred <= 0)
			break;

		grant = MIN(cred, conn->need);

		rlb_conn_respond(rlb, conn, grant);

		cred -= grant;
	}

	*_cred = cred;
}

static void
rlb_meminfo_dispatch(td_rlb_t *rlb, void *data)
{
	td_rlb_meminfo_t *m = data;
	long long us, hi, cred, dirty, pend;

	/* we run only once per m->period */

	us = rlb_usec_since(rlb, &m->ts);
	if (us / 1000 > m->period) {
		rlb_meminfo_scan(m);
		m->ts = rlb->now;
	}

	/* uncongested credit:
	   memory below hi watermark minus pending I/O */

	hi  = m->total;
	hi *= m->limit_hi;
	hi /= 100;

	dirty = m->dirty + m->writeback;

	cred  = hi - dirty;
	cred *= 1000;

	pend  = rlb_pending(rlb);
	cred -= pend;

	m->congested = rlb_meminfo_test_high(rlb, m, cred);

	DBG(3, "dirty=%lld (%lld) pend=%llu cred=%lld %s",
	    dirty, dirty * 100 / m->total, pend, cred,
	    m->congested ? "congested" : "");

	if (!m->congested) {
		rlb_meminfo_dispatch_low(rlb, m, &cred);

		m->congested = rlb_meminfo_test_high(rlb, m, cred);
	}

	if (m->congested)
		m->valve.ops->dispatch(rlb, m->valve.data);
}

static struct ratelimit_ops rlb_meminfo_ops = {
	.usage    = rlb_meminfo_usage,
	.create   = rlb_meminfo_create,
	.destroy  = rlb_meminfo_destroy,
	.info     = rlb_meminfo_info,

	.settimeo = rlb_meminfo_settimeo,
	.timeout  = rlb_meminfo_timeout,
	.dispatch = rlb_meminfo_dispatch,
};

/*
 * main loop
 */

static void
rlb_info(td_rlb_t *rlb)
{
	rlb->valve.ops->info(rlb, rlb->valve.data);

	rlb_conn_infos(rlb);
}

static sigset_t rlb_sigunblock;
static sigset_t rlb_sigpending;

static void
rlb_sigmark(int signo)
{
	INFO("Caught SIG%d", signo);
	sigaddset(&rlb_sigpending, signo);
}

static int
rlb_siginit(void)
{
	struct sigaction sa_ignore  = { .sa_handler = SIG_IGN };
	struct sigaction sa_pending = { .sa_handler = rlb_sigmark };
	sigset_t sigmask;
	int err = 0;

	if (!err)
		err = sigaction(SIGPIPE, &sa_ignore, NULL);
	if (!err)
		err = sigaction(SIGINT,  &sa_pending, NULL);
	if (!err)
		err = sigaction(SIGTERM, &sa_pending, NULL);
	if (!err)
		err = sigaction(SIGUSR1, &sa_pending, NULL);
	if (err) {
		err = -errno;
		goto fail;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGUSR1);

	err = sigprocmask(SIG_BLOCK, &sigmask, &rlb_sigunblock);
	if (err) {
		err = -errno;
		goto fail;
	}

fail:
	return err;
}

static int
rlb_main_signaled(td_rlb_t *rlb)
{
	if (sigismember(&rlb_sigpending, SIGUSR1))
		rlb_info(rlb);

	if (sigismember(&rlb_sigpending, SIGINT) ||
	    sigismember(&rlb_sigpending, SIGTERM))
		return -EINTR;

	return 0;
}


static struct ratelimit_ops *
rlb_find_valve(const char *name)
{
	struct ratelimit_ops *ops = NULL;

	switch (name[0]) {
#if 0
	case 'l':
		if (!strcmp(name, "leaky"))
			ops = &rlb_leaky_ops;
		break;
#endif

	case 't':
		if (!strcmp(name, "token"))
			ops = &rlb_token_ops;
		break;

	case 'm':
		if (!strcmp(name, "meminfo"))
			ops = &rlb_meminfo_ops;
		break;
	}

	return ops;
}

static int
rlb_main_iterate(td_rlb_t *rlb)
{
	td_rlb_conn_t *conn, *next;
	struct timeval *tv;
	struct timespec _ts, *ts = &_ts;
	int nfds, err;
	fd_set rfds;

	FD_ZERO(&rfds);
	nfds = 0;

	if (stdin) {
		FD_SET(STDIN_FILENO, &rfds);
		nfds = MAX(nfds, STDIN_FILENO);
	}

	if (rlb->sock >= 0) {
		FD_SET(rlb->sock, &rfds);
		nfds = MAX(nfds, rlb->sock);
	}

	rlb_for_each_conn(conn, rlb) {
		FD_SET(conn->sock, &rfds);
		nfds = MAX(nfds, conn->sock);
	}

	rlb->valve.ops->settimeo(rlb, &tv, rlb->valve.data);
	if (tv) {
		TIMEVAL_TO_TIMESPEC(tv, ts);
	} else
		ts = NULL;

	rlb->ts = rlb->now;

	nfds = pselect(nfds + 1, &rfds, NULL, NULL, ts, &rlb_sigunblock);
	if (nfds < 0) {
		err = -errno;
		if (err != -EINTR)
			PERROR("select");
		goto fail;
	}

	gettimeofday(&rlb->now, NULL);

	if (!nfds) {
		BUG_ON(!ts);
		rlb->valve.ops->timeout(rlb, rlb->valve.data);
	}

	if (nfds) {
		rlb_for_each_conn_safe(conn, next, rlb)
			if (FD_ISSET(conn->sock, &rfds)) {
				rlb_conn_receive(rlb, conn);
				if (!--nfds)
					break;
			}

		rlb->valve.ops->dispatch(rlb, rlb->valve.data);
	}

	if (unlikely(nfds)) {
		if (FD_ISSET(STDIN_FILENO, &rfds)) {
			getc(stdin);
			rlb_info(rlb);
			nfds--;
		}
	}

	if (unlikely(nfds)) {
		if (FD_ISSET(rlb->sock, &rfds)) {
			rlb_accept_conn(rlb);
			nfds--;
		}
	}

	BUG_ON(nfds);
	err = 0;
fail:
	return err;
}

static int
rlb_main_run(td_rlb_t *rlb)
{
	int err;

	do {
		err = rlb_main_iterate(rlb);
		if (err) {
			if (err != -EINTR)
				break;

			err = rlb_main_signaled(rlb);
			if (err) {
				err = 0;
				break;
			}
		}

	} while (rlb->sock >= 0 || !list_empty(&rlb->open));

	return err;
}

static void
rlb_shutdown(td_rlb_t *rlb)
{
	td_rlb_conn_t *conn, *next;

	rlb_for_each_conn_safe(conn, next, rlb)
		rlb_conn_close(rlb, conn);

	rlb_sock_close(rlb);
}

static void
rlb_usage(td_rlb_t *rlb, const char *prog, FILE *stream)
{
	fprintf(stream, "Usage: %s <name>", prog);

	if (rlb && rlb->valve.ops)
		rlb->valve.ops->usage(rlb, stream, rlb->valve.data);
	else
		fprintf(stream,
			" {-t|--type}={token|meminfo}"
			" [-h|--help] [-D|--debug=<n>]");

	fprintf(stream, "\n");
}

static void
rlb_destroy(td_rlb_t *rlb)
{
	rlb_shutdown(rlb);

	if (rlb->valve.data) {
		rlb->valve.ops->destroy(rlb, rlb->valve.data);
		rlb->valve.data = NULL;
	}

	if (rlb->name) {
		free(rlb->name);
		rlb->name = NULL;
	}
}

static int
rlb_create(td_rlb_t *rlb, const char *name)
{
	int i, err;

	memset(rlb, 0, sizeof(*rlb));
	INIT_LIST_HEAD(&rlb->open);
	INIT_LIST_HEAD(&rlb->wait);
	rlb->sock = -1;

	for (i = RLB_CONN_MAX - 1; i >= 0; i--)
		rlb_conn_free(rlb, &rlb->connv[i]);

	rlb->name = strdup(name);
	if (!rlb->name) {
		err = -errno;
		goto fail;
	}

	err = rlb_sock_open(rlb);
	if (err)
		goto fail;

	gettimeofday(&rlb->now, NULL);

	return 0;

fail:
	WARN("err = %d", err);
	rlb_destroy(rlb);
	return err;
}

static int
rlb_create_valve(td_rlb_t *rlb, struct rlb_valve *v,
		 const char *name, int argc, char **argv)
{
	struct ratelimit_ops *ops;
	int err;

	ops = rlb_find_valve(name);
	if (!ops) {
		ERR("No such driver: %s", name);
		err = -ESRCH;
		goto fail;
	}

	v->ops = ops;

	err = v->ops->create(rlb, argc, argv, &v->data);

fail:
	return err;
}

static void
rlb_openlog(const char *name, int facility)
{
	static char ident[32];

	snprintf(ident, sizeof(ident), "%s[%d]", name, getpid());
	ident[sizeof(ident)-1] = 0;

	openlog(ident, 0, facility);

	rlb_vlog = vsyslog;
}

int
main(int argc, char **argv)
{
	td_rlb_t _rlb, *rlb;
	const char *prog, *type;
	int err;

	setbuf(stdin, NULL);
	setlinebuf(stderr);

	rlb      = NULL;
	prog     = basename(argv[0]);
	type     = NULL;
	rlb_vlog = rlb_vlog_vfprintf;

	do {
		const struct option longopts[] = {
			{ "help",        0, NULL, 'h' },
			{ "type",        1, NULL, 't' },
			{ "debug",       0, NULL, 'D' },
			{ NULL,          0, NULL,  0  },
		};
		int c;

		c = getopt_long(argc, argv, "ht:D:", longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'h':
			rlb_usage(NULL, prog, stdout);
			return 0;

		case 't':
			type = optarg;
			break;

		case 'D':
			debug = strtoul(optarg, NULL, 0);
			break;

		case '?':
			goto usage;

		default:
			BUG();
		}

	} while (1);

	if (!type)
		goto usage;

	if (argc - optind < 1)
		goto usage;

	err = rlb_siginit();
	if (err)
		goto fail;

	err = rlb_create(&_rlb, argv[optind++]);
	if (err)
		goto fail;

	rlb = &_rlb;

	rlb_argv_shift(&optind, &argc, &argv);

	err = rlb_create_valve(rlb, &rlb->valve, type, argc, argv);
	if (err) {
		if (err == -EINVAL)
			goto usage;
		goto fail;
	}

	if (!debug) {
		err = daemon(0, 0);
		if (err)
			goto fail;

		stdin = stdout = stderr = NULL;
		rlb_openlog(prog, LOG_DAEMON);
	}

	INFO("TD ratelimit bridge: %s, pid %d", rlb->path, getpid());

	rlb_info(rlb);

	err = rlb_main_run(rlb);

	if (err)
		INFO("Exiting with status %d", -err);

fail:
	if (rlb)
		rlb_destroy(rlb);

	return -err;

usage:
	rlb_usage(rlb, prog, stderr);
	err = -EINVAL;
	goto fail;
}
