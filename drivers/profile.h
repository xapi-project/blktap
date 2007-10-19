/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 */

#ifndef __TAP_PROFILE_H__
#define __TAP_PROFILE_H__

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <inttypes.h>

//#define PROFILING
//#define LOGGING

#define TAPPROF_IN  1
#define TAPPROF_OUT 2

struct profile_times {
	char    *fn_name;
	uint64_t in, out_sum, cnt;
};

struct profile_info {
	FILE                 *log;
	int                   size;
	char                 *name;
	unsigned long long    seq;
	struct profile_times *pt;
};

#ifdef PROFILING

static inline void
tp_open(struct profile_info *prof, char *tap_name, char *log_name, int size)
{
	memset(prof, 0, sizeof(struct profile_info));
#ifdef LOGGING
	prof->log  = fopen(log_name, "w");
#endif
	prof->size = size;
	prof->name = strdup(tap_name);
	prof->pt   = malloc(sizeof(struct profile_times) * prof->size);
	if (prof->pt)
		memset(prof->pt, 0, sizeof(struct profile_times) * prof->size);
}

static inline void
tp_close(struct profile_info *prof)
{
	int i;
	struct profile_times *pt;

	for (i = 0; i < prof->size; i++) {
		pt = &prof->pt[i];
		if (pt->fn_name) {
			syslog(LOG_DEBUG, "%s: %s: cnt: %llu, avg time: %llu\n",
			       prof->name, pt->fn_name, pt->cnt, 
			       ((pt->cnt) ? (pt->out_sum / pt->cnt) : 0));
			free(pt->fn_name);
		}
	}

#ifdef LOGGING
	if (prof->log)
		fclose(prof->log);
#endif
	free(prof->name);
	if (prof->pt)
		free(prof->pt);
}

static inline u64
tp_get_id(struct profile_info *prof)
{
	return prof->seq++;
}

static inline int
tp_fn_id(struct profile_info *prof, const char *name)
{
	int i;
	struct profile_times *pt;

	for (i = 0; i < prof->size; i++) {
		pt = &prof->pt[i];
		if (!pt->fn_name)
			return i;
		if (!strcmp(pt->fn_name, name))
			return i;
	}

	return prof->size - 1;
}

static inline void
__tp_in(struct profile_info *prof, const char *func)
{
	long long _time;
	int idx = tp_fn_id(prof, func);
	struct profile_times *pt = &prof->pt[idx];

	if (!pt->fn_name) 
		pt->fn_name = strdup(func);

	asm volatile(".byte 0x0f, 0x31" : "=A" (_time));
	pt->in = _time;
}

#define tp_in(prof) __tp_in(prof, __func__)

static inline void
__tp_out(struct profile_info *prof, const char *func)	
{
	long long _time;
	int idx = tp_fn_id(prof, func);
	struct profile_times *pt = &prof->pt[idx];

	if (!pt->fn_name || !pt->in)
		return;

	asm volatile(".byte 0x0f, 0x31" : "=A" (_time));
	pt->cnt++;
	pt->out_sum += (_time - pt->in);
	pt->in       = 0;
}

#define tp_out(prof) __tp_out(prof, __func__)

static inline void
__tp_log(struct profile_info *prof, u64 id, const char *func, int direction)
{
	long long _time;
	asm volatile(".byte 0x0f, 0x31" : "=A" (_time));

	if (direction == TAPPROF_IN)
		__tp_in(prof, func);
	else 
		__tp_out(prof, func);

#ifdef LOGGING
        if (prof->log)
	        fprintf(prof->log, "%s: %s: %llu, %lld\n", func, 
			((direction == TAPPROF_IN) ? "in" : "out"), id, _time);
#endif
}

#define tp_log(prof, id, direction) __tp_log(prof, id, __func__, direction)

#else
#define tp_open(prof, tname, lname, size)  ((void)0)
#define tp_close(prof)                     ((void)0)
#define tp_in(prof)                        ((void)0)
#define tp_out(prof)                       ((void)0)
#define tp_log(prof, sec, direction)       ((void)0)
#endif

#define BSHIFT          9
#define BALIGN          (1 << BSHIFT)
#define BUF_PAD         (BALIGN << 1)
#define BUF_SIZE        (1 << 20)
#define MAX_ENTRY_LEN   256

struct bhandle {
	char          *p;
	uint64_t       cnt;
	char           buf[BUF_SIZE + BUF_PAD];
};

#define B_ALIGN(buf) (char *)((((long)buf + (BALIGN - 1)) >> BSHIFT) << BSHIFT)
#define B_END(buf)   (char *)((long)B_ALIGN(buf) + BUF_SIZE)

#define BLOG(h, _f, _a...)                                                     \
do {                                                                           \
	int _len;                                                              \
	struct timeval t;                                                      \
                                                                               \
	if (!h.p || (h.p + MAX_ENTRY_LEN) > B_END(h.buf))                      \
		h.p = B_ALIGN(h.buf);                                          \
                                                                               \
	gettimeofday(&t, NULL);                                                \
	_len = snprintf(h.p, MAX_ENTRY_LEN - 2, "%"PRIu64":%ld.%ld: "          \
			_f, h.cnt, t.tv_sec, t.tv_usec, ##_a);                 \
	_len = (_len < MAX_ENTRY_LEN ? _len : MAX_ENTRY_LEN - 1);              \
	h.p[_len] = '\0';                                                      \
                                                                               \
	h.cnt++;                                                               \
	h.p += _len;                                                           \
} while (0)

#define BDUMP(file, h)                                                         \
do {                                                                           \
	int fd;                                                                \
	char *name;                                                            \
                                                                               \
	if (asprintf(&name, "%s.%d", file, getpid()) == -1)                    \
		break;                                                         \
                                                                               \
	fd = open(name, O_CREAT | O_TRUNC |                                    \
		  O_WRONLY | O_DIRECT | O_NONBLOCK, 0644);                     \
                                                                               \
	free(name);                                                            \
	if (fd == -1)                                                          \
		break;                                                         \
                                                                               \
	write(fd, B_ALIGN(h.buf), BUF_SIZE);                                   \
	close(fd);                                                             \
} while (0)

#define MAX_ERROR_MESSAGES 16

struct error {
	int          cnt;
	int          err;
	char        *func;
	char         msg[MAX_ENTRY_LEN];
};

struct ehandle {
	int          cnt;
	int          dropped;
	struct error errors[MAX_ERROR_MESSAGES];
};

#define LOG_ERROR(h, _err, _f, _a...)                                          \
do {                                                                           \
	struct error *e;                                                       \
	struct timeval t;                                                      \
	int i, __err, done = 0;                                                \
                                                                               \
	__err = ((_err) > 0 ? (_err) : -(_err));                               \
                                                                               \
	for (i = 0; i < h.cnt; i++) {                                          \
		e = &h.errors[i];                                              \
		if (e->err == __err && e->func == __func__) {                  \
			e->cnt++;                                              \
			done = 1;                                              \
			break;                                                 \
		}                                                              \
	}                                                                      \
                                                                               \
	if (done)                                                              \
		break;                                                         \
                                                                               \
	if (h.cnt >= MAX_ERROR_MESSAGES) {                                     \
		h.dropped++;                                                   \
		break;                                                         \
	}                                                                      \
	                                                                       \
	gettimeofday(&t, NULL);                                                \
	e = &h.errors[h.cnt];                                                  \
                                                                               \
	if (snprintf(e->msg, MAX_ENTRY_LEN, "%ld.%ld: "                        \
		     _f, t.tv_sec, t.tv_usec, ##_a) > 0) {                     \
		h.cnt++;                                                       \
		e->cnt++;                                                      \
		e->err  = __err;                                               \
		e->func = (char *)__func__;                                    \
	}                                                                      \
} while (0)

#define PRINT_ERRORS(h)                                                        \
do {                                                                           \
	int i;                                                                 \
	struct error *e;                                                       \
                                                                               \
	for (i = 0; i < h.cnt; i++) {                                          \
		e = &h.errors[i];                                              \
		syslog(LOG_INFO, "TAPDISK ERROR: errno %d: at %s "             \
		       "(cnt = %d): %s\n", e->err, e->func, e->cnt, e->msg);   \
	}                                                                      \
                                                                               \
	if (h.dropped)                                                         \
		syslog(LOG_INFO, "TAPDISK ERROR: %d other error messages "     \
		       "dropped\n", h.dropped);                                \
} while (0)

static struct ehandle tap_errors;
#define TAP_ERROR(_err, _f, _a...) LOG_ERROR(tap_errors, _err, _f, ##_a)
#define TAP_PRINT_ERRORS()         PRINT_ERRORS(tap_errors)

#endif
