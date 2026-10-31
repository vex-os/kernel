#pragma once

#include <usr/vex/spawn.h>

#define SPAWN_SETSID    __k_SPAWN_SETSID
#define SPAWN_SETPGID   __k_SPAWN_SETPGID
#define SPAWN_EMPTY_FDS __k_SPAWN_EMPTY_FDS

#define SPAWN_KNOWN_FLAGS __k_SPAWN_KNOWN_FLAGS
#define SPAWN_MAX_ACTIONS __k_SPAWN_MAX_ACTIONS

#define SPAWN_OPEN  __k_SPAWN_OPEN
#define SPAWN_CLOSE __k_SPAWN_CLOSE
#define SPAWN_DUP2  __k_SPAWN_DUP2

typedef struct __k_spawn_args spawn_args_t;
typedef union __k_spawn_action spawn_action_t;
