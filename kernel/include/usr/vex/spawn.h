#ifndef USR_VEX_SPAWN_H
#define USR_VEX_SPAWN_H

#include <vex/types.h>

#define __k_SPAWN_SETSID    0x01 /* The child leads a new session and a new group */
#define __k_SPAWN_SETPGID   0x02 /* The child joins the group args->pgid, or one of its own when it is 0 */
#define __k_SPAWN_EMPTY_FDS 0x04 /* The child starts with an empty descriptor table instead of a copy */

/* Not with __k_SPAWN_SETSID: a session leader cannot join a group */
#define __k_SPAWN_KNOWN_FLAGS (__k_SPAWN_SETSID | __k_SPAWN_SETPGID | __k_SPAWN_EMPTY_FDS)

#define __k_SPAWN_MAX_ACTIONS 1024

/* The operations run in order on the child table, with the credentials of
 * the caller, before the image loads. A failed operation fails the call:
 * no child appears. A close of a descriptor that is not open is not an
 * error, and a dup2 with both numbers equal only checks that it is open */
#define __k_SPAWN_OPEN  1 /* open(path, flags, mode) on the number fd */
#define __k_SPAWN_CLOSE 2
#define __k_SPAWN_DUP2  3

struct __k_spawn_action_open {
    __k_uint32_t op; /* __k_SPAWN_OPEN */
    __k_int32_t dirfd;
    __k_uint32_t open_flags; /* Without __k_O_CLOEXEC, which becomes the fd flag */
    __k_uint32_t mode;
    __k_int32_t fd;
    __k_int32_t reserved; /* Must be 0 */
    __k_uintptr_t path;
};

struct __k_spawn_action_close {
    __k_uint32_t op; /* __k_SPAWN_CLOSE */
    __k_int32_t fd;
};

struct __k_spawn_action_dup2 {
    __k_uint32_t op; /* __k_SPAWN_DUP2 */
    __k_int32_t oldfd;
    __k_int32_t newfd;
};

union __k_spawn_action {
    __k_uint32_t op;
    struct __k_spawn_action_open open;
    struct __k_spawn_action_close close;
    struct __k_spawn_action_dup2 dup2;
};

struct __k_spawn_args {
    __k_uintptr_t path;
    __k_uintptr_t argv;
    __k_uintptr_t envp;
    __k_uint64_t flags;
    __k_pid_t pgid; /* With __k_SPAWN_SETPGID */
    __k_uintptr_t actions;
    __k_size_t action_count;
};

#endif /* USR_VEX_SPAWN_H */
