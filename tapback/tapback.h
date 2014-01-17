/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#ifndef __TAPBACK_H__
#define __TAPBACK_H__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <xen/xen.h>
#include <xen/io/xenbus.h>
#include <xen/event_channel.h>
#include <xen/grant_table.h>
#include <xenstore.h>

#include "list.h"
#include "tap-ctl.h"
#include "blktap3.h"

/* TODO */
#define __printf(_f, _a)        __attribute__((format (printf, _f, _a)))
#define __scanf(_f, _a)         __attribute__((format (scanf, _f, _a)))

#define ARRAY_SIZE(_a) (sizeof(_a)/sizeof((_a)[0]))

void tapback_log(int prio, const char *fmt, ...);
void (*tapback_vlog) (int prio, const char *fmt, va_list ap);

#define TAPBACK_LOG "/var/log/tapback.log"

/**
 * Fills in the buffer with a pretty timestamp.
 *
 * FIXME There may be an easier/simpler way to do this.
 */
int pretty_time(char *buf, unsigned char buf_len);

#define DBG(device, _fmt, _args...)  \
    do {                                                                    \
        vbd_t *_device = (vbd_t*)(device);                                  \
        char buf[32];                                                       \
        pretty_time(buf, ARRAY_SIZE(buf));                                  \
        if (_device) {                                                      \
            tapback_log(LOG_DEBUG, "%s %s:%d %d/%d "_fmt, buf, __FILE__,    \
                    __LINE__, _device->domid, _device->devid, ##_args);     \
         } else {                                                           \
            tapback_log(LOG_DEBUG, "%s %s:%d "_fmt, buf, __FILE__, __LINE__,\
                ##_args);                                                   \
        }                                                                   \
    } while (0)

#define INFO(device, _fmt, _args...) \
    do {                                                                    \
        vbd_t *_device = (vbd_t*)(device);                                  \
        char buf[32];                                                       \
        pretty_time(buf, ARRAY_SIZE(buf));                                  \
        if (_device) {                                                      \
            tapback_log(LOG_INFO, "%s %s:%d %d/%d "_fmt, buf, __FILE__,     \
                    __LINE__, _device->domid, _device->devid, ##_args);     \
         } else {                                                           \
            tapback_log(LOG_INFO, "%s %s:%d "_fmt, buf, __FILE__, __LINE__, \
                ##_args);                                                   \
        }                                                                   \
    } while (0)

#define WARN(device, _fmt, _args...) \
    do {                                                                    \
        vbd_t *_device = (vbd_t*)(device);                                  \
        char buf[32];                                                       \
        pretty_time(buf, ARRAY_SIZE(buf));                                  \
        if (_device) {                                                      \
            tapback_log(LOG_WARNING, "%s %s:%d %d/%d "_fmt, buf, __FILE__,  \
                    __LINE__, _device->domid, _device->devid, ##_args);     \
         } else {                                                           \
            tapback_log(LOG_WARNING, "%s %s:%d "_fmt, buf, __FILE__,        \
                    __LINE__, ##_args);                                     \
        }                                                                   \
    } while (0)

#define WARN_ON(_cond, fmt, ...)    \
    if (unlikely(_cond)) {          \
        printf(fmt, ##__VA_ARGS__); \
    }

#define ASSERT(p)                                                   \
    do {                                                            \
        if (!(p)) {                                                 \
            tapback_log(LOG_WARNING,                                \
                    "tapback-err:%s:%d: FAILED ASSERTION: '%s'\n",  \
                    __FILE__, __LINE__, #p);                        \
            abort();                                                \
        }                                                           \
    } while (0)

/*
 * Pre-defined XenStore path components used for running the XenBus protocol.
 */
#define XENSTORE_BACKEND			"backend"
#define BLKTAP3_BACKEND_NAME		"vbd"
#define BLKTAP3_BACKEND_PATH		XENSTORE_BACKEND"/"BLKTAP3_BACKEND_NAME
#define BLKTAP3_BACKEND_TOKEN		XENSTORE_BACKEND"-"BLKTAP3_BACKEND_NAME
#define BLKTAP3_FRONTEND_TOKEN		"otherend-state"
#define PHYS_DEV_KEY                "physical-device"
#define HOTPLUG_STATUS_KEY			"hotplug-status"

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

	/*
	 * FIXME rename to backend_state
	 */
    XenbusState state;

	XenbusState frontend_state;

} vbd_t;

/**
 * The collection of all necessary handles and descriptors.
 */
struct _blktap3_daemon {

    /**
     * A handle to XenStore.
     */
    struct xs_handle *xs;

    /**
     * The list of virtual block devices.
     *
     * TODO We sometimes have to scan the whole list to find the device/domain
     * we're interested in, should we optimize this? E.g. use a hash table
     * for O(1) access?
     * TODO Replace with a hash table (hcreate etc.)?
     */
    struct list_head devices;

    /**
     * TODO From xen/include/public/io/blkif.h: "The maximum supported size of
     * the request ring buffer"
     */
    int max_ring_page_order;

    /**
     * Unix domain socket for controlling the daemon.
     */
    int ctrl_sock;
};

extern struct _blktap3_daemon blktap3_daemon;

#define tapback_backend_for_each_device(_device, _next)                 \
	list_for_each_entry_safe(_device, _next, &blktap3_daemon.devices,   \
            backend_entry)

/**
 * Iterates over all devices and returns the one for which the condition is
 * true.
 */
#define tapback_backend_find_device(_device, _cond)     \
do {                                                    \
    vbd_t *__next;                                      \
    int found = 0;                                      \
    tapback_backend_for_each_device(_device, __next) {  \
        if (_cond) {                                    \
            found = 1;                                  \
            break;                                      \
        }                                               \
    }                                                   \
    if (!found)                                         \
        _device = NULL;                                 \
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
 * @returns a buffer containing the value, or NULL on error
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
 * @returns a buffer containing the value, or NULL on error
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
 * @returns 0 on success, an error code otherwise
 *
 * XXX Only called by tapback_read_watch
 */
int
tapback_backend_handle_otherend_watch(const char * const path);

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
 * an error code otherwise
 *
 * TODO We only care about changes on the domid/devid component, as this
 * signifies device creation/removal. Changes to paths such as
 * "backend/vbd3/29/51712/mode" or "backend/vbd3/29/51712/removable" are
 * currently uninteresting and we shouldn't do anything.
 *
 * XXX Only called by tapback_read_watch.
 */
int
tapback_backend_handle_backend_watch(char * const path);

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

#endif /* __TAPBACK_H__ */
