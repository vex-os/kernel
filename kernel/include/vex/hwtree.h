#pragma once

#include <usr/vex/hwtree.h>
#include <vex/compiler.h>
#include <vex/feed.h>
#include <vex/types.h>

typedef struct device_s device_t;

#define HWTREE_STATE_PRESENT   __k_HWTREE_STATE_PRESENT
#define HWTREE_STATE_PROBING   __k_HWTREE_STATE_PROBING
#define HWTREE_STATE_WAITING   __k_HWTREE_STATE_WAITING
#define HWTREE_STATE_BOUND     __k_HWTREE_STATE_BOUND
#define HWTREE_STATE_DETACHING __k_HWTREE_STATE_DETACHING
#define HWTREE_STATE_GONE      __k_HWTREE_STATE_GONE

#define FEED_TYPE_HWTREE_SNAPSHOT_BEGIN   __k_FEED_TYPE_HWTREE_SNAPSHOT_BEGIN
#define FEED_TYPE_HWTREE_SNAPSHOT_END     __k_FEED_TYPE_HWTREE_SNAPSHOT_END
#define FEED_TYPE_HWTREE_ADD              __k_FEED_TYPE_HWTREE_ADD
#define FEED_TYPE_HWTREE_REMOVE           __k_FEED_TYPE_HWTREE_REMOVE
#define FEED_TYPE_HWTREE_BIND             __k_FEED_TYPE_HWTREE_BIND
#define FEED_TYPE_HWTREE_UNBIND           __k_FEED_TYPE_HWTREE_UNBIND
#define FEED_TYPE_HWTREE_WAITING          __k_FEED_TYPE_HWTREE_WAITING
#define FEED_TYPE_HWTREE_ATTACH_FAILED    __k_FEED_TYPE_HWTREE_ATTACH_FAILED
#define FEED_TYPE_HWTREE_INTERFACE        __k_FEED_TYPE_HWTREE_INTERFACE
#define FEED_TYPE_HWTREE_INTERFACE_REMOVE __k_FEED_TYPE_HWTREE_INTERFACE_REMOVE

#define HWTREE_FLAG_TRUNCATED __k_HWTREE_FLAG_TRUNCATED

#define HWTREE_FIELDS_MAX __k_HWTREE_FIELDS_MAX

#define HWTREE_RESNAPSHOT __k_HWTREE_RESNAPSHOT
#define HWTREE_RESCAN     __k_HWTREE_RESCAN
#define HWTREE_UNBIND     __k_HWTREE_UNBIND
#define HWTREE_EJECT      __k_HWTREE_EJECT

typedef struct __k_hwtree_record hwtree_record_t;

static_assert(sizeof(hwtree_record_t) % FEED_ALIGN == 0);

/// Adds a key=value field to the record
/// @note A field that doesn't fit is left out whole, and the record gets HWTREE_FLAG_TRUNCATED
void hwtree_put(hwtree_record_t* restrict record, const char* restrict key, const char* restrict fmt, ...) __printflike(3, 4);

/// Tells the readers of /dev/hwtree that an interface of a node appeared
void hwtree_interface_added(const device_t* restrict device, const char* restrict kind, const char* restrict name);

void hwtree_interface_removed(const device_t* restrict device, const char* restrict name);
