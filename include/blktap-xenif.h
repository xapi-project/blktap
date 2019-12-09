#ifndef __BLKTAPXENIF_H__
#define __BLKTAPXENIF_H__

/* Make sure that __XEN_INTERFACE_VERSION__ is defined high enough
 * that xen/io/ring.h will not attempt to define xen_mb() and friends.
 * This file needs to be included before anything which includes
 * xen/io/ring.h */
#define __XEN_INTERFACE_VERSION__ 0x00040000

/* Header files for the stable event channel and grant table
 * interfaces */
#include <xenevtchn.h>
#include <xengnttab.h>

/* Get the correct defines for memory barriers - do not fall
 * back to those provided by the kernel */
#define xen_mb()  asm volatile ("mfence" ::: "memory")
#define xen_rmb() asm volatile ("" ::: "memory")
#define xen_wmb() asm volatile ("" ::: "memory")

#endif /* __BLKTAPXENIF_H__ */
