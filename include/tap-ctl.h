/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
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

#ifndef __TAP_CTL_H__
#define __TAP_CTL_H__

#include <syslog.h>
#include <errno.h>
#include <sys/time.h>
#include <tapdisk-message.h>
#include <list.h>

#include <xen/xen.h>
#include <xen/grant_table.h>
#include <xen/event_channel.h>

#define TAPCTL_COMM_RETRY_TIMEOUT 120

extern int tap_ctl_debug;

#ifdef TAPCTL
#define DBG(_f, _a...)				\
	do {					\
		if (tap_ctl_debug)		\
			printf(_f, ##_a);	\
	} while (0)

#define DPRINTF(_f, _a...) syslog(LOG_INFO, _f, ##_a)
#define EPRINTF(_f, _a...) syslog(LOG_ERR, "tap-err:%s: " _f, __func__, ##_a)
#define  PERROR(_f, _a...) syslog(LOG_ERR, "tap-err:%s: " _f ": %s", __func__, ##_a, \
				  strerror(errno))
#endif

void tap_ctl_version(int *major, int *minor);
int tap_ctl_kernel_version(int *major, int *minor);

int tap_ctl_check_blktap(const char **message);
int tap_ctl_check_version(const char **message);
int tap_ctl_check(const char **message);

int tap_ctl_connect(const char *path, int *socket);
int tap_ctl_connect_id(int id, int *socket);
int tap_ctl_read_raw(int fd, void *buf, size_t sz, struct timeval *timeout);
int tap_ctl_read_message(int fd, tapdisk_message_t *message,
			 struct timeval *timeout);
int tap_ctl_write_message(int fd, tapdisk_message_t *message,
			  struct timeval *timeout);
int tap_ctl_send_and_receive(int fd, tapdisk_message_t *message,
			     struct timeval *timeout);
int tap_ctl_connect_send_and_receive(int id,
				     tapdisk_message_t *message,
				     struct timeval *timeout);
char *tap_ctl_socket_name(int id);

typedef struct {
	pid_t       pid;
	int         minor;
	int         state;
	char       *type;
	char       *path;

	struct list_head entry;
} tap_list_t;

#define tap_list_for_each_entry(_pos, _head) \
	list_for_each_entry(_pos, _head, entry)

#define tap_list_for_each_entry_safe(_pos, _n, _head) \
	list_for_each_entry_safe(_pos, _n, _head, entry)

int tap_ctl_list(struct list_head *list);
int tap_ctl_list_pid(pid_t pid, struct list_head *list);
void tap_ctl_list_free(struct list_head *list);

int tap_ctl_find_minor(const char *type, const char *path);

int tap_ctl_allocate(int *minor, char **devname);
int tap_ctl_free(const int minor);

int tap_ctl_create(const char *params, char **devname, int flags, 
		int prt_minor, char *secondary, int timeout, const char *slice, const char *logpath);
int tap_ctl_destroy(const int id, const int minor, int force,
		    struct timeval *timeout);

int tap_ctl_spawn(const char *slice);
pid_t tap_ctl_get_pid(const int id);

int tap_ctl_attach(const int id, const int minor);
int tap_ctl_detach(const int id, const int minor);

int tap_ctl_open(const int id, const int minor, const char *params, int flags,
		const int prt_minor, const char *secondary, int timeout, const char *logpath);
int tap_ctl_close(const int id, const int minor, const int force,
		  struct timeval *timeout);

/**
 * Pauses the VBD.
 */
int tap_ctl_pause(const int id, const int minor, struct timeval *timeout);

/**
 * Unpauses the VBD
 *
 * @param pid the process ID of the tapdisk
 * @param flags TODO
 * @param secondary TODO
 * @param uuid
 * @param new_params the new VDI to use (type:/path/to/file), optional
 */
int tap_ctl_unpause(const int id, const int minor, const char *params,
		int flags, char *secondary, const char *logpath);

ssize_t tap_ctl_stats(pid_t pid, int minor, char *buf, size_t size);
int tap_ctl_stats_fwrite(pid_t pid, int minor, FILE *out);

int tap_ctl_blk_major(void);

/**
 * Instructs a tapdisk to connect to the shared ring.
 *
 * @param pid the process ID of the tapdisk that should connect to the shared
 * ring
 * @param domid the domain ID of the guest VM
 * @param devid the device ID
 * @param poll_duration polling duration (microseconds; 0 means no polling)
 * @param poll_idle_threshold CPU idle threshold above which we poll
 * @param grefs the grant references
 * @param order number of grant references, expressed as a 2's order
 * @param port event channel port
 * @param proto the protocol: native (XENIO_BLKIF_PROTO_NATIVE),
 * x86 (XENIO_BLKIF_PROTO_X86_32), or x64 (XENIO_BLKIF_PROTO_X86_64)
 * @param pool a string used as an identifier to group two or more VBDs
 * beloning to the same tapdisk process. For VBDs with the same pool name, a
 * single event channel is used.
 * @param minor
 * @returns 0 on success, a negative error code otherwise
 */
int tap_ctl_connect_xenblkif(const pid_t pid, const domid_t domid, const int
		devid, int poll_duration, int poll_idle_threshold,
		const grant_ref_t * grefs, const int order, const evtchn_port_t
		port, int proto, const char *pool, const int minor);

/**
 * Instructs a tapdisk to disconnect from the shared ring.
 *
 * @param pid process ID of the tapdisk
 * @param domid the ID of the guest VM
 * @param devid the device ID of the virtual block device
 * @param timeout timeout to wait, if NULL the function will wait indefinitely
 * @returns 0 on success, a negative error code otherwise
 */
int tap_ctl_disconnect_xenblkif(const pid_t pid, const domid_t domid,
        const int devid, struct timeval *timeout);

/**
 * Retrieves virtual disk information from a tapdisk.
 *
 * @param pid the process ID of the tapdisk process
 * @param sectors output parameter that receives the number of sectors
 * @param sector_size output parameter that receives the size of the sector
 * @param info TODO ?
 * @param minor
 *
 */
int tap_ctl_info(pid_t pid, unsigned long long *sectors, unsigned int
		*sector_size, unsigned int *info, const int minor);

/**
 * Parses a type:/path/to/file string, storing the type and path to the output
 * parameters. Upon successful completion the caller must free @type and @path,
 * otherwise their values are undefined.
 *
 * @param params type:/path/to/file to parse
 * @param type output parameter the receives the type
 * @param path output paramter that receives the path
 * @returns 0 on success
 */
int
parse_params(const char *params, char **type, char **path);

#endif
