/* Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 *
 * XenSource proprietary code.
 */

#ifndef _RELATIVE_PATH_H_
#define _RELATIVE_PATH_H_

#include <syslog.h>

#define DELIMITER    '/'
#define MAX_NAME_LEN 1000

#define EPRINTF(_f, _a...) syslog(LOG_ERR, "tap-err:%s: " _f, __func__, ##_a)

/*
 * returns a relative path from @src to @dest
 * result should be freed
 */
char *relative_path_to(char *src, char *dest, int *err);

#endif
