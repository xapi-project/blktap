/* Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 */

#ifndef _TAPDISK_LOG_H_
#define _TAPDISK_LOG_H_

#define TLOG_WARN       0
#define TLOG_INFO       1
#define TLOG_DBG        2

void open_tlog(char *file, size_t bytes, int level, int append);
void close_tlog(void);
void tlog_flush(void);
void tlog_print_errors(void);

void __tlog_write(int level, const char *func, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
void __tlog_error(int err, const char *func, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));

#define tlog_write(_level, _f, _a...)			\
	__tlog_write(_level, __func__, _f, ##_a)

#define tlog_error(_err, _f, _a...)			\
	__tlog_error(_err, __func__, _f, ##_a)

#endif
