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

#ifndef __TAPBACK_H__
#define __TAPBACK_H__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <search.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <xen/xen.h>
#include <xen/io/xenbus.h>
#include <xen/event_channel.h>
#include <xen/grant_table.h>
#include <xenstore.h>

#include "debug.h"
#include "util.h"
#include "list.h"
#include "tap-ctl.h"
#include "blktap3.h"

/* TODO */
#define __printf(_f, _a)        __attribute__((format (printf, _f, _a)))
#define __scanf(_f, _a)         __attribute__((format (scanf, _f, _a)))

extern const char tapback_name[];

extern unsigned log_level;

/**
 * Fills in the buffer with a pretty timestamp.
 *
 * FIXME There may be an easier/simpler way to do this.
 */
int pretty_time(char *buf, unsigned char buf_len);

#define DBG(device, _fmt, _args...)  \
    do {                                                                    \
        vbd_t *_device = (vbd_t*)(device);                                  \
        if (_device) {                                                      \
            syslog(LOG_DEBUG, "%s:%d %d "_fmt, __FILE__, __LINE__,          \
                    _device->devid, ##_args);                               \
         } else {                                                           \
            syslog(LOG_DEBUG, "%s:%d "_fmt, __FILE__, __LINE__, ##_args);   \
        }                                                                   \
    } while (0)

#define INFO(device, _fmt, _args...)  \
    do {                                                                    \
        vbd_t *_device = (vbd_t*)(device);                                  \
        if (_device) {                                                      \
            syslog(LOG_INFO, "%s:%d %d "_fmt, __FILE__, __LINE__,           \
                    _device->devid, ##_args);                               \
         } else {                                                           \
            syslog(LOG_INFO, "%s:%d "_fmt, __FILE__, __LINE__, ##_args);    \
        }                                                                   \
    } while (0)

#define WARN(device, _fmt, _args...)  \
    do {                                                                    \
        vbd_t *_device = (vbd_t*)(device);                                  \
        if (_device) {                                                      \
            syslog(LOG_WARNING, "%s:%d %d "_fmt, __FILE__, __LINE__,        \
                    _device->devid, ##_args);                               \
         } else {                                                           \
            syslog(LOG_WARNING, "%s:%d "_fmt, __FILE__, __LINE__, ##_args); \
        }                                                                   \
    } while (0)


/*
 * Pre-defined XenStore path components used for running the XenBus protocol.
 */
#define XENSTORE_BACKEND			"backend"
#define PHYS_DEV_KEY                "physical-device"
#define HOTPLUG_STATUS_KEY			"hotplug-status"
#define MODE_KEY					"mode"
#define POLLING_DURATION	"polling-duration"
#define POLLING_IDLE_THRESHOLD	"polling-idle-threshold"

/*
 * TODO Put the rest of the front-end nodes defined in blkif.h here and group
 * them. e.g. FRONTEND_NODE_xxx.
 */
#define RING_REF                "ring-ref"
#define RING_PAGE_ORDER         "ring-page-order"
#define EVENT_CHANNEL           "event-channel"
#define FEAT_PERSIST            "feature-persistent"
#define PROTO                   "protocol"
#define FRONTEND_KEY            "frontend"

struct backend_master {
    void *slaves;
};

int
compare(const void *pa, const void *pb);

struct backend_slave {
    union {
        struct {
            /**
             * The list of virtual block devices.
             */
            struct list_head devices;

            /**
             * TODO From xen/include/public/io/blkif.h: "The maximum supported
             * size of the request ring buffer"
             */
            int max_ring_page_order;
        } slave;
        struct {
            pid_t pid;
            domid_t domid;
        } master;
    };
};

/**
 * The collection of all necessary handles and descriptors.
 */
typedef struct backend {

    /**
     * XenStore key name, e.g. vbd, vbd3 etc.
     */
    char *name;

    /*
     * FIXME document
     */
    char *path;

    /**
     * A handle to XenStore.
     */
    struct xs_handle *xs;

    union {
        struct backend_master master;
        struct backend_slave slave;
    };

    /**
     * Unix domain socket for controlling the daemon.
     */
    int ctrl_sock;

    struct sockaddr_un local;

    /**
     * Domain ID in which tapback is running.
     */
	domid_t domid;

    /**
     * Domain ID served by this tapback. If it's zero it means that this is the
     * master tapback.
     */
    domid_t slave_domid;

    char *frontend_token;
    char *backend_token;

	/**
	 * for linked lists
	 */
	struct list_head entry;

    char *pidfile;

	/**
	 * Tells whether we support write I/O barriers.
	 */
	bool barrier;
} backend_t;

/**
 * A Virtual Block Device (VBD), represents a block device in a guest VM.
 * Contains all relevant information.
 */
typedef struct vbd {

    /**
     * Device name, as retrieved from XenStore at probe-time.
     */
    char *name;

    /**
     * The device ID. Same as vbd.name, we keep it around because the tapdisk
     * control library wants it as an int and not as a string.
     */
    int devid;

    /**
     * For linked lists.
     */
     struct list_head backend_entry;

    /**
     * The domain ID this VBD belongs to.
     */
    domid_t domid;

    /**
     * The root directory in XenStore for this VBD. This is where all
     * directories and key/value pairs related to this VBD are stored.
     */
    char *frontend_path;

    /**
     * XenStore path to the VBD's state. This is just
     * vbd.frontend_path + "/state", we keep it around so we don't have to
     * allocate/free memory all the time.
     */
    char *frontend_state_path;

	/**
	 * Indicates whether the "hotplug-status" key has received the "connected"
	 * value.
	 */
	bool hotplug_status_connected;

    /**
     * Indicates whether the tapdisk is connected to the shared ring.
     */
    bool connected;

    /**
     * Descriptor of the tapdisk process serving this virtual block device. We
     * need this until the end of the VBD's lifetime in order to disconnect
     * the tapdisk from the shared ring.
     */
    tap_list_t *tap;

    /*
     * XXX We keep sector_size, sectors, and info because we need to
     * communicate them to the front-end not only when the front-end goes to
     * XenbusStateInitialised, but to XenbusStateConnected as well.
     */

    /**
     * Sector size, supplied by the tapdisk, communicated to blkfront.
     */
    unsigned int sector_size;

    /**
     * Number of sectors, supplied by the tapdisk, communicated to blkfront.
     */
    unsigned long long sectors;

    /**
     * VDISK_???, defined in include/xen/interface/io/blkif.h.
     */
    unsigned int info;

    int major;
	int minor;

	bool mode;
	bool cdrom;

	/**
	 * Polling duration in microseconds. 0 means no polling.
	 */
	int polling_duration;

	/**
	 * Idle CPU threshold above which polling is permitted.
	 */
	int polling_idle_threshold;

	/*
	 * FIXME rename to backend_state
	 */
    XenbusState state;

	XenbusState frontend_state;

	backend_t *backend;
} vbd_t;

#define tapback_backend_for_each_device(_backend, _device, _next)			\
    ASSERT(!tapback_is_master(_backend));                                   \
	list_for_each_entry_safe(_device, _next, &backend->slave.slave.devices,	\
            backend_entry)

/**
 * Iterates over all devices and returns the one for which the condition is
 * true.
 */
#define tapback_backend_find_device(_backend, _device, _cond)		\
do {																\
    vbd_t *__next;													\
    int found = 0;													\
    ASSERT(!tapback_is_master(_backend));                           \
    tapback_backend_for_each_device(_backend, _device, __next) {	\
        if (_cond) {												\
            found = 1;												\
            break;													\
        }															\
    }																\
    if (!found)														\
        _device = NULL;												\
} while (0)

/**
 * Retrieves the XenStore value of the specified key of the VBD's front-end.
 * The caller must free the returned buffer.
 *
 * @param device the VBD
 * @param xst XenStore transaction
 * @param path key under the front-end directory
 * @returns a buffer containing the value, or NULL on error
 */
char *
tapback_device_read_otherend(vbd_t * const device, xs_transaction_t xst,
        const char * const path);

/**
 * Writes to XenStore backened/tapback/<domid>/<devname>/@key = @fmt.
 *
 * @param device the VBD
 * @param xst XenStore transaction
 * @param key the key to write to
 * @param mkread TODO
 * @param fmt format
 * @returns 0 on success, an negative error code otherwise
 */
int
tapback_device_printf(vbd_t * const device, xs_transaction_t xst,
        const char * const key, const bool mkread, const char * const fmt,
        ...);

/**
 * Reads the specified XenStore path under the front-end directory in a
 * scanf-like manner.
 *
 * @param device the VBD
 * @param xst XenStore transaction
 * @param path XenStore path to read
 * @param fmt format
 */
int
tapback_device_scanf_otherend(vbd_t * const device, xs_transaction_t xst,
        const char * const path, const char * const fmt, ...);

/**
 * Retrieves the value of the specified of the device from XenStore,
 * i.e. backend/tapback/<domid>/<devname>/@path
 * The caller must free the returned buffer.
 *
 * @param device the VBD
 * @param xst XenStore transaction
 * @param path the XenStore key
 * @returns a buffer containing the value, or NULL on error
 */
char *
tapback_device_read(const vbd_t * const device, xs_transaction_t xst,
        const char * const path);

/**
 * Reads the specified XenStore path. The caller must free the returned buffer.
 *
 * @param xs handle to XenStore
 * @param xst XenStore transaction
 * @param fmt format
 * @param ap arguments
 * @returns a buffer containing the value, or NULL on error, sets errno
 */
char *
tapback_xs_vread(struct xs_handle * const xs, xs_transaction_t xst,
        const char * const fmt, va_list ap);

/**
 * Reads the specified XenStore path. The caller must free the returned buffer.
 *
 * @param xs handle to XenStore
 * @param xst XenStore transaction
 * @param fmt format
 * @returns a buffer containing the value, or NULL on error, sets errno
 */
__printf(3, 4)
char *
tapback_xs_read(struct xs_handle * const xs, xs_transaction_t xst,
        const char * const fmt, ...);

/**
 * Act in response to a change in the front-end XenStore path.
 *
 * TODO We only care about changes on the front-end's state. Document this.
 * Also, executed the body of this function (blkback_frontend_changed) iff a
 * change occured on the state, otherwise immediatelly return.
 *
 * @param path the front-end's XenStore path that changed
 * @returns 0 on success, a positive error code otherwise
 *
 * XXX Only called by tapback_read_watch
 */
int
tapback_backend_handle_otherend_watch(backend_t *backend,
		const char * const path);

/**
 * Act in response to a change in the back-end directory in XenStore.
 *
 * If the path is "/backend" or "/backend/<backend name>", all devices are
 * probed. Otherwise, the path should be
 * "backend/<backend name>/<domid>/<device name>"
 * (i.e. backend/<backend name>/1/51712), and in this case this specific device
 * is probed.
 *
 * @param path the back-end's XenStore path that changed @returns 0 on success,
 * a positive error code otherwise
 *
 * XXX Only called by tapback_read_watch.
 */
int
tapback_backend_handle_backend_watch(backend_t *backend,
		char * const path);

/**
 * Converts XenbusState values to a printable string, e.g. XenbusStateConnected
 * corresponds to "connected".
 *
 * @param xbs the XenbusState to convert
 * @returns a printable string
 */
char *
xenbus_strstate(const XenbusState xbs);

/**
 * Switches the back-end state of the device by writing to XenStore.
 *
 * @param device the VBD
 * @param state the state to switch to
 * @returns 0 on success, an error code otherwise
 */
int
xenbus_switch_state(vbd_t * const device, const XenbusState state);

/**
 * Acts on changes in the front-end state.
 *
 * TODO The back-end blindly follows the front-ends state transitions, should
 * we check whether unexpected transitions are performed?
 *
 * @param device the VBD whose front-end state changed
 * @param state the new state
 * @returns 0 on success, an error code otherwise
 *
 * FIXME Add a function for each front-end state transition to make the code
 * more readable (e.g. frontend_initialising, frontend_connected, etc.).
 */
int
frontend_changed(vbd_t * const device, const XenbusState state);

/**
 * Returns the current domain ID or -errno.
 */
int
get_my_domid(struct xs_handle * const xs, xs_transaction_t xst);

bool
tapback_is_master(const backend_t *backend);

struct backend_slave*
tapback_find_slave(const backend_t *master_backend, const domid_t domid);

/**
 * Tells whether a XenStore key exists. XXX This function temporarily modifies
 * the path argument, so it's not thread-safe.
 *
 * @param xs handle to XenStore
 * @param xst XenStore transaction
 * @param path path to check for existence
 * @param len optional argument that if non-NULL, it's value is used to limit
 * the length of the path to be checked
 *
 * Returns 0 if the key does not exist, 1 if it does, and -errno on error.
 */
int
tapback_xs_exists(struct xs_handle * const xs, xs_transaction_t xst,
        char *path, const int *len);

void
tapback_backend_destroy(backend_t *backend);

bool verbose(void);

#endif /* __TAPBACK_H__ */
