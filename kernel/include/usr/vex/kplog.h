#ifndef USR_VEX_KPLOG_H
#define USR_VEX_KPLOG_H

#include <vex/feed.h>
#include <vex/ioctl.h>

#define __k_KP_EMERG    0x0000 /* Level: The system is unusable, it's joever */
#define __k_KP_ALERT    0x0001 /* Level: Action must be taken immediately, usually unrecoverable */
#define __k_KP_CRITICAL 0x0002 /* Level: Critical conditions, probably unrecoverable */
#define __k_KP_ERROR    0x0003 /* Level: Error conditions, probably recoverable */
#define __k_KP_WARNING  0x0004 /* Level: Warning messages, definitely recoverable */
#define __k_KP_NOTICE   0x0005 /* Level: Normal but important messages */
#define __k_KP_INFORM   0x0006 /* Level: Informational messages */
#define __k_KP_VERBOSE  0x0007 /* Level: Debug-level messages */

#define __k_KP_CORE     0x0000 /* Subsystem: general, unassigned kprintf calls */
#define __k_KP_ARCH     0x0001 /* Subsystem: architecture-specific code */
#define __k_KP_MM       0x0002 /* Subsystem: memory management */
#define __k_KP_PROC     0x0003 /* Subsystem: processes, threads, scheduling and system calls */
#define __k_KP_IRQ      0x0004 /* Subsystem: interrupt handling */
#define __k_KP_TIME     0x0005 /* Subsystem: timekeeping */
#define __k_KP_FIRMWARE 0x0006 /* Subsystem: firmware handling */
#define __k_KP_DEVICE   0x0007 /* Subsystem: device tree and driver binding */
#define __k_KP_BUS      0x0008 /* Subsystem: bus drivers, e.g. PCI, ISA */
#define __k_KP_DEV      0x0009 /* Subsystem: block, character, tty and other interfaces */
#define __k_KP_DRIVERS  0x000A /* Subsystem: device drivers, e.g. uart16550 */
#define __k_KP_FS       0x000B /* Subsystem: VFS and filesystem drivers */
#define __k_KP_EXEC     0x000C /* Subsystem: binary format registry */
#define __k_KP_CRYPTO   0x000D /* Subsystem: cryptographic nonsense */
#define __k_KP_IPC      0x000E /* Subsystem: interprocess communications */
#define __k_KP_RANDOM   0x000F /* Subsystem: entropy pool and random number generators */
#define __k_KP_USER     0x00FF /* Subsystem: userspace writes to /dev/kplog */

#define __k_FEED_TYPE_KPLOG __k_FEED_TYPE_DEF(0) /* struct __k_kplog_record */

#define __k_KPLOG_FLAG_TRUNCATED 0x0001 /* The message was longer than a record holds */

#define __k_KPLOG_TEXT_MAX 512U

struct __k_kplog_record {
    struct __k_feed_header feed;
    __k_uint16_t subsystem;
    __k_uint16_t level;
    __k_uint32_t text_size;
    char text[__k_KPLOG_TEXT_MAX]; /* This is NOT zero-terminated */
};

struct __k_kplog_info {
    __k_uint64_t seq_first; /* Of the oldest record the ring holds */
    __k_uint64_t seq_next;  /* Of the record to come */
};

#define __k_KPLOG_SEEK_FIRST        __k_IOCTL_VOID('L', 0x01)
#define __k_KPLOG_SEEK_END          __k_IOCTL_VOID('L', 0x02) /* To the record to come */
#define __k_KPLOG_SEEK_SEQ          __k_IOCTL_WRITE(sizeof(__k_uint64_t), 'L', 0x03)
#define __k_KPLOG_GET_INFO          __k_IOCTL_READ(sizeof(struct __k_kplog_info), 'L', 0x04)
#define __k_KPLOG_GET_CONSOLE_LEVEL __k_IOCTL_READ(sizeof(__k_int32_t), 'L', 0x05)
#define __k_KPLOG_SET_CONSOLE_LEVEL __k_IOCTL_WRITE(sizeof(__k_int32_t), 'L', 0x06)

#endif /* USR_VEX_KPLOG_H */
