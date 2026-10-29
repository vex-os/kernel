#ifndef USR_VEX_CAPABILITY_H
#define USR_VEX_CAPABILITY_H

#include <vex/types.h>

/* NOTE: __k_CAP bit numbers are same as ones
 * defined in Linux, so that any file operations
 * done there transfer to Vex without issues */

#define __k_CAP_CHOWN          0
#define __k_CAP_DAC_OVERRIDE   1 /* Bypass file permission checks */
#define __k_CAP_FOWNER         3 /* Act as the owner of any file */
#define __k_CAP_FSETID         4 /* Keep setuid and setgid bits a change would clear */
#define __k_CAP_KILL           5 /* Signal any process */
#define __k_CAP_SETGID         6
#define __k_CAP_SETUID         7
#define __k_CAP_SYS_ADMIN      21 /* Catch-all for administration, trusted.* extended attributes among it */
#define __k_CAP_SYS_NICE       23 /* Lower the nice value of a process, and change processes of other users */
#define __k_CAP_SYS_RESOURCE   24 /* Raise a hard resource limit */
#define __k_CAP_SYS_TIME       25 /* Step the realtime clock */
#define __k_CAP_SYS_TTY_CONFIG 26 /* Configure serial ports and what they are bound to */
#define __k_CAP_SETFCAP        31 /* Set file capabilities */
#define __k_CAP_SYSLOG         34 /* Write to the kernel log */

/* Vex-only capabilities start at 48 */

#define __k_CAP_MOUNT        48
#define __k_CAP_DEVICE       49 /* Rescan, unbind and eject devices through /dev/hwtree */
#define __k_CAP_VM_NORESERVE 50 /* Map memory with MAP_NORESERVE, that takes nothing from the commit limit */

#define __k_CAP_TO_MASK(cap) ((__k_uint64_t)(1) << (cap))

#define __k_CAPS_KEEP 0x0001 /* Keep the permitted set when no uid is 0 anymore */

/* File capabilities live in the security.capability extended
 * attribute, laid out the way Linux has them, little-endian */

#define __k_XATTR_NAME_CAPS "security.capability"

#define __k_VFS_CAP_REVISION_MASK   0xFF000000
#define __k_VFS_CAP_REVISION_1      0x01000000 /* 32-bit sets, no rootid */
#define __k_VFS_CAP_REVISION_2      0x02000000 /* 64-bit sets, no rootid */
#define __k_VFS_CAP_REVISION_3      0x03000000 /* 64-bit sets and rootid */
#define __k_VFS_CAP_FLAGS_EFFECTIVE 0x00000001

#define __k_VFS_CAP_SIZE_1 12
#define __k_VFS_CAP_SIZE_2 20
#define __k_VFS_CAP_SIZE_3 24

struct __k_vfs_cap_word {
    __k_uint32_t permitted;
    __k_uint32_t inheritable;
};

struct __k_vfs_cap_data {
    __k_uint32_t magic_etc;
    struct __k_vfs_cap_word data[2]; /* Low half of the sets first */
    __k_uint32_t rootid;             /* Revision 3 only */
};

struct __k_caps {
    __k_uint64_t effective;
    __k_uint64_t permitted;
    __k_uint64_t ambient;  /* Survives exec of a program that is not setuid or setgid */
    __k_uint64_t bounding; /* No exec ever grants more than this */
    __k_uint32_t flags;
    __k_uint32_t reserved;
};

#endif /* USR_VEX_CAPABILITY_H */
