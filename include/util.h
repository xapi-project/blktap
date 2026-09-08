/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __TAPDISK_UTIL_H__
#define __TAPDISK_UTIL_H__

#include <stddef.h>
#include <string.h>

#define ARRAY_SIZE(_a) (sizeof(_a)/sizeof((_a)[0]))

/*
 * Strncpy variant that guarantees to terminate the string
 */
static inline void
safe_strncpy(char *dest, const char *src, size_t n)
{
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
	if (n > 0) {
		strncpy(dest, src, n - 1);
		dest[n - 1] = '\0';
	}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

#endif /* __TAPDISK_UTIL_H__ */
