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
#include <unistd.h>
#include <string.h>
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

#endif
