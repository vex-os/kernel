#ifndef USR_VEX_TYPES_H
#define USR_VEX_TYPES_H

#include <arch/types.h>

#if defined(__INT8_TYPE__) && defined(__INT16_TYPE__) && defined(__INT32_TYPE__) && defined(__INT64_TYPE__) && defined(__UINT8_TYPE__) \
    && defined(__UINT16_TYPE__) && defined(__UINT32_TYPE__) && defined(__UINT64_TYPE__)
#define __k_SINT8_TYPE  __INT8_TYPE__
#define __k_SINT16_TYPE __INT16_TYPE__
#define __k_SINT32_TYPE __INT32_TYPE__
#define __k_SINT64_TYPE __INT64_TYPE__
#define __k_UINT8_TYPE  __UINT8_TYPE__
#define __k_UINT16_TYPE __UINT16_TYPE__
#define __k_UINT32_TYPE __UINT32_TYPE__
#define __k_UINT64_TYPE __UINT64_TYPE__
#define __k_SSIZE_TYPE  __INT64_TYPE__
#define __k_SIZE_TYPE   __UINT64_TYPE__
#elif defined(__cplusplus) || defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stddef.h>
#include <stdint.h>
#define __k_SINT8_TYPE  int8_t
#define __k_SINT16_TYPE int16_t
#define __k_SINT32_TYPE int32_t
#define __k_SINT64_TYPE int64_t
#define __k_UINT8_TYPE  uint8_t
#define __k_UINT16_TYPE uint16_t
#define __k_UINT32_TYPE uint32_t
#define __k_UINT64_TYPE uint64_t
#define __k_SSIZE_TYPE  int64_t
#define __k_SIZE_TYPE   uint64_t
#else
#include <limits.h>
#if UCHAR_MAX == 0xFF
#define __k_SINT8_TYPE signed char
#define __k_UINT8_TYPE unsigned char
#else
#error "unsupported char width"
#endif
#if USHRT_MAX == 0xFFFF
#define __k_SINT16_TYPE signed short
#define __k_UINT16_TYPE unsigned short
#else
#error "unsupported short width"
#endif
#if UINT_MAX == 0xFFFFFFFFU
#define __k_SINT32_TYPE signed int
#define __k_UINT32_TYPE unsigned int
#elif ULONG_MAX == 0xFFFFFFFFUL
#define __k_SINT32_TYPE signed long
#define __k_UINT32_TYPE unsigned long
#else
#error "unsupported 32-bit type"
#endif
#if ULONG_MAX == 0xFFFFFFFFFFFFFFFFUL
#define __k_SINT64_TYPE signed long
#define __k_UINT64_TYPE unsigned long
#define __k_SSIZE_TYPE  signed long
#define __k_SIZE_TYPE   unsigned long
#elif defined(ULLONG_MAX) && ULLONG_MAX == 0xFFFFFFFFFFFFFFFFULL
#define __k_SINT64_TYPE signed long long
#define __k_UINT64_TYPE unsigned long long
#define __k_SSIZE_TYPE  signed long long
#define __k_SIZE_TYPE   unsigned long long
#else
#error "64-bit integer type unavailable in this C89 environment"
#endif
#endif

#if __k_POINTER_BITS == 64
#define __k_SINTPTR_TYPE __k_SINT64_TYPE
#define __k_UINTPTR_TYPE __k_UINT64_TYPE
#elif __k_POINTER_BITS == 32
#define __k_INTPTR_TYPE  __k_INT32_TYPE
#define __k_UINTPTR_TYPE __k_UINT32_TYPE
#else
#error "unsupported pointer width"
#endif

typedef __k_SINT8_TYPE __k_int8_t;
typedef __k_SINT16_TYPE __k_int16_t;
typedef __k_SINT32_TYPE __k_int32_t;
typedef __k_SINT64_TYPE __k_int64_t;

typedef __k_UINT8_TYPE __k_uint8_t;
typedef __k_UINT16_TYPE __k_uint16_t;
typedef __k_UINT32_TYPE __k_uint32_t;
typedef __k_UINT64_TYPE __k_uint64_t;

typedef __k_SINTPTR_TYPE __k_intptr_t;
typedef __k_UINTPTR_TYPE __k_uintptr_t;
typedef __k_SSIZE_TYPE __k_ssize_t;
typedef __k_SIZE_TYPE __k_size_t;

typedef __k_SINT32_TYPE __k_id_t;
typedef __k_SINT32_TYPE __k_gid_t;
typedef __k_SINT32_TYPE __k_key_t;
typedef __k_SINT32_TYPE __k_pid_t;
typedef __k_SINT32_TYPE __k_uid_t;

typedef __k_SINT64_TYPE __k_blkcnt_t;
typedef __k_SINT64_TYPE __k_blksize_t;
typedef __k_UINT64_TYPE __k_dev_t;
typedef __k_SINT64_TYPE __k_nlink_t;
typedef __k_SINT64_TYPE __k_off_t;
typedef __k_UINT32_TYPE __k_mode_t;
typedef __k_UINT32_TYPE __k_reclen_t;
typedef __k_UINT64_TYPE __k_fsblkcnt_t;
typedef __k_UINT64_TYPE __k_fsfilcnt_t;
typedef __k_UINT64_TYPE __k_ino_t;

typedef __k_SINT32_TYPE __k_clockid_t;
typedef __k_SINT64_TYPE __k_suseconds_t;
typedef __k_SINT32_TYPE __k_timer_t;
typedef __k_SINT64_TYPE __k_clock_t;
typedef __k_SINT64_TYPE __k_time_t;

#endif /* USR_VEX_TYPES_H */
