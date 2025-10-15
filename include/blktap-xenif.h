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
/* from <xen-barrier.h> in 4.20.0 */

#define xen_barrier() asm volatile ( "" : : : "memory")

#if defined(__i386__)
#define xen_mb()  asm volatile ( "lock addl $0, -4(%%esp)" ::: "memory" )
#define xen_rmb() xen_barrier()
#define xen_wmb() xen_barrier()
#elif defined(__x86_64__)
#define xen_mb()  asm volatile ( "lock addl $0, -32(%%rsp)" ::: "memory" )
#define xen_rmb() xen_barrier()
#define xen_wmb() xen_barrier()
#elif defined(__arm__)
#define xen_mb()   asm volatile ("dmb" : : : "memory")
#define xen_rmb()  asm volatile ("dmb" : : : "memory")
#define xen_wmb()  asm volatile ("dmb" : : : "memory")
#elif defined(__aarch64__)
#define xen_mb()   asm volatile ("dmb sy" : : : "memory")
#define xen_rmb()  asm volatile ("dmb sy" : : : "memory")
#define xen_wmb()  asm volatile ("dmb sy" : : : "memory")
#else
#error "Define barriers"
#endif

#endif /* __BLKTAPXENIF_H__ */
