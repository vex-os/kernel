#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <usr/vex/types.h>
#include <vex/compiler.h>

#if __STDC_VERSION__ < 199901L
#error "C99 or later is required"
#endif

// Kernel ABI defines these but never aliases
// standard C99-provided types to __k_ counterparts
// so we have to ensure that the types have equal sizes here

static_assert(sizeof(uint8_t) == sizeof(__k_uint8_t));
static_assert(sizeof(uint16_t) == sizeof(__k_uint16_t));
static_assert(sizeof(uint32_t) == sizeof(__k_uint32_t));
static_assert(sizeof(uint64_t) == sizeof(__k_uint64_t));

static_assert(sizeof(int8_t) == sizeof(__k_int8_t));
static_assert(sizeof(int16_t) == sizeof(__k_int16_t));
static_assert(sizeof(int32_t) == sizeof(__k_int32_t));
static_assert(sizeof(int64_t) == sizeof(__k_int64_t));

static_assert(sizeof(intptr_t) == sizeof(__k_intptr_t));
static_assert(sizeof(uintptr_t) == sizeof(__k_uintptr_t));
static_assert(sizeof(size_t) == sizeof(__k_size_t));

typedef __k_ssize_t ssize_t;

typedef __k_id_t id_t;
typedef __k_gid_t gid_t;
typedef __k_key_t key_t;
typedef __k_pid_t pid_t;
typedef __k_uid_t uid_t;

typedef __k_blkcnt_t blkcnt_t;
typedef __k_blksize_t blksize_t;
typedef __k_dev_t dev_t;
typedef __k_nlink_t nlink_t;
typedef __k_off_t off_t;
typedef __k_mode_t mode_t;
typedef __k_reclen_t reclen_t;
typedef __k_fsblkcnt_t fsblkcnt_t;
typedef __k_fsfilcnt_t fsfilcnt_t;
typedef __k_ino_t ino_t;

typedef __k_clockid_t clockid_t;
typedef __k_suseconds_t suseconds_t;
typedef __k_timer_t timer_t;
typedef __k_clock_t clock_t;
typedef __k_time_t time_t;

typedef __uint128_t uint128_t;

typedef union user_addr_u user_addr_t;
typedef struct virt_addr_s virt_addr_t;
typedef struct phys_addr_s phys_addr_t;

union __transparent_union user_addr_u {
    uintptr_t value;
};

struct virt_addr_s {
    uintptr_t value;
};

struct phys_addr_s {
    uintptr_t value;
};

static_assert(sizeof(uintptr_t) == sizeof(user_addr_t));
static_assert(sizeof(uintptr_t) == sizeof(virt_addr_t));
static_assert(sizeof(uintptr_t) == sizeof(phys_addr_t));
