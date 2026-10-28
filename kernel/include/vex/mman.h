#pragma once

#include <usr/vex/mman.h>

#define PROT_NONE  __k_PROT_NONE
#define PROT_READ  __k_PROT_READ
#define PROT_WRITE __k_PROT_WRITE
#define PROT_EXEC  __k_PROT_EXEC

#define MAP_SHARED          __k_MAP_SHARED
#define MAP_PRIVATE         __k_MAP_PRIVATE
#define MAP_FIXED           __k_MAP_FIXED
#define MAP_ANONYMOUS       __k_MAP_ANONYMOUS
#define MAP_NORESERVE       __k_MAP_NORESERVE
#define MAP_POPULATE        __k_MAP_POPULATE
#define MAP_STACK           __k_MAP_STACK
#define MAP_FIXED_NOREPLACE __k_MAP_FIXED_NOREPLACE

#define MS_ASYNC      __k_MS_ASYNC
#define MS_INVALIDATE __k_MS_INVALIDATE
#define MS_SYNC       __k_MS_SYNC
