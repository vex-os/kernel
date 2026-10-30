#pragma once

#include <usr/vex/kplog.h>
#include <vex/feed.h>

#define KP_EMERG    __k_KP_EMERG
#define KP_ALERT    __k_KP_ALERT
#define KP_CRITICAL __k_KP_CRITICAL
#define KP_ERROR    __k_KP_ERROR
#define KP_WARNING  __k_KP_WARNING
#define KP_NOTICE   __k_KP_NOTICE
#define KP_INFORM   __k_KP_INFORM
#define KP_VERBOSE  __k_KP_VERBOSE

#define KP_CORE     __k_KP_CORE
#define KP_ARCH     __k_KP_ARCH
#define KP_MM       __k_KP_MM
#define KP_PROC     __k_KP_PROC
#define KP_IRQ      __k_KP_IRQ
#define KP_TIME     __k_KP_TIME
#define KP_FIRMWARE __k_KP_FIRMWARE
#define KP_DEVICE   __k_KP_DEVICE
#define KP_BUS      __k_KP_BUS
#define KP_DEV      __k_KP_DEV
#define KP_DRIVERS  __k_KP_DRIVERS
#define KP_FS       __k_KP_FS
#define KP_EXEC     __k_KP_EXEC
#define KP_CRYPTO   __k_KP_CRYPTO
#define KP_IPC      __k_KP_IPC
#define KP_RANDOM   __k_KP_RANDOM
#define KP_USER     __k_KP_USER

#define FEED_TYPE_KPLOG __k_FEED_TYPE_KPLOG

#define KPLOG_FLAG_TRUNCATED __k_KPLOG_FLAG_TRUNCATED

#define KPLOG_TEXT_MAX __k_KPLOG_TEXT_MAX

#define KPLOG_SEEK_FIRST        __k_KPLOG_SEEK_FIRST
#define KPLOG_SEEK_END          __k_KPLOG_SEEK_END
#define KPLOG_SEEK_SEQ          __k_KPLOG_SEEK_SEQ
#define KPLOG_GET_INFO          __k_KPLOG_GET_INFO
#define KPLOG_GET_CONSOLE_LEVEL __k_KPLOG_GET_CONSOLE_LEVEL
#define KPLOG_SET_CONSOLE_LEVEL __k_KPLOG_SET_CONSOLE_LEVEL

typedef struct __k_kplog_record kplog_record_t;
typedef struct __k_kplog_info kplog_info_t;

static_assert(sizeof(kplog_record_t) % FEED_ALIGN == 0);
