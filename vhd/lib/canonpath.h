/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CANONPATH_H_
#define _CANONPATH_H_

/*
 * returns a canonical path from @path to @resolved_path
 */
char *canonpath(const char *path, char *resolved_path, size_t dest_size);

#endif
