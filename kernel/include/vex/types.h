#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <usr/vex/types.h>

#if __STDC_VERSION__ < 199901L
#error "C99 or later is required"
#endif

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
