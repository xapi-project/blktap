/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __TAPDISK_DEBUG_H__
#define __TAPDISK_DEBUG_H__

#include <syslog.h>
#include <stdlib.h>

#ifdef NDEBUG
#define ASSERT(condition)	\
	do {					\
		;					\
	} while(0);
#else
#define ASSERT(condition)													\
	do {																	\
		if (!(condition)) {													\
			syslog(LOG_ERR, "%s:%d: %s: Assertion `%s' failed.", __FILE__,	\
					__LINE__, __func__, __STRING(condition));				\
			abort();														\
		}																	\
	} while (0);
#endif

#endif /* __TAPDISK_DEBUG_H__ */
