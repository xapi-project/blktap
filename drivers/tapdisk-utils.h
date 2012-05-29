/* 
 * Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _TAPDISK_UTILS_H_
#define _TAPDISK_UTILS_H_

#include <inttypes.h>
#include <sys/time.h>

#define MAX_NAME_LEN          1000
#define TD_SYSLOG_IDENT_MAX   32
#define TD_SYSLOG_STRTIME_LEN 15

int tapdisk_syslog_facility(const char *);
char* tapdisk_syslog_ident(const char *);
size_t tapdisk_syslog_strftime(char *, size_t, const struct timeval *);
size_t tapdisk_syslog_strftv(char *, size_t, const struct timeval *);
int tapdisk_set_resource_limits(void);
int tapdisk_namedup(char **, const char *);
int tapdisk_parse_disk_type(const char *, char **, int *);
int tapdisk_get_image_size(int, uint64_t *, uint32_t *);
int tapdisk_linux_version(void);
uint64_t ntohll(uint64_t);
#define htonll ntohll

#endif
