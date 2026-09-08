/*
 * Copyright (c) 2017, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CBT_UTIL_PRIV_H_
#define _CBT_UTIL_PRIV_H_

typedef int (*cbt_util_func_t) (int, char **);

struct command {
	char			*name;
	cbt_util_func_t	func;
};

struct command *
get_command(char *command);

void
help(void);

#endif /*_CBT_UTIL_PRIV_H_*/
