#pragma once

#include <usr/vex/capability.h>

#define CAP_CHOWN          __k_CAP_CHOWN
#define CAP_DAC_OVERRIDE   __k_CAP_DAC_OVERRIDE
#define CAP_FOWNER         __k_CAP_FOWNER
#define CAP_FSETID         __k_CAP_FSETID
#define CAP_KILL           __k_CAP_KILL
#define CAP_SETGID         __k_CAP_SETGID
#define CAP_SETUID         __k_CAP_SETUID
#define CAP_SYS_CHROOT     __k_CAP_SYS_CHROOT
#define CAP_SYS_ADMIN      __k_CAP_SYS_ADMIN
#define CAP_SYS_NICE       __k_CAP_SYS_NICE
#define CAP_SYS_RESOURCE   __k_CAP_SYS_RESOURCE
#define CAP_SYS_TIME       __k_CAP_SYS_TIME
#define CAP_SYS_TTY_CONFIG __k_CAP_SYS_TTY_CONFIG
#define CAP_MKNOD          __k_CAP_MKNOD
#define CAP_SETFCAP        __k_CAP_SETFCAP
#define CAP_SYSLOG         __k_CAP_SYSLOG
#define CAP_MOUNT          __k_CAP_MOUNT
#define CAP_DEVICE         __k_CAP_DEVICE
#define CAP_VM_NORESERVE   __k_CAP_VM_NORESERVE

#define CAP_TO_MASK(cap) __k_CAP_TO_MASK(cap)
#define CAP_FULL_SET     UINT64_MAX

#define CAPS_KEEP __k_CAPS_KEEP

#define XATTR_NAME_CAPS __k_XATTR_NAME_CAPS

#define VFS_CAP_REVISION_MASK   __k_VFS_CAP_REVISION_MASK
#define VFS_CAP_REVISION_1      __k_VFS_CAP_REVISION_1
#define VFS_CAP_REVISION_2      __k_VFS_CAP_REVISION_2
#define VFS_CAP_REVISION_3      __k_VFS_CAP_REVISION_3
#define VFS_CAP_FLAGS_EFFECTIVE __k_VFS_CAP_FLAGS_EFFECTIVE

#define VFS_CAP_SIZE_1 __k_VFS_CAP_SIZE_1
#define VFS_CAP_SIZE_2 __k_VFS_CAP_SIZE_2
#define VFS_CAP_SIZE_3 __k_VFS_CAP_SIZE_3

typedef struct __k_caps caps_t;
typedef struct __k_vfs_cap_data vfs_cap_data_t;
