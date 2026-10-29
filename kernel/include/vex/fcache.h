#pragma once

#include <vex/compiler.h>
#include <vex/mutex.h>
#include <vex/radix.h>
#include <vex/types.h>

typedef struct pmap_s pmap_t;
typedef struct vfs_s vfs_t;
typedef struct vnode_s vnode_t;

typedef struct fcache_s fcache_t;
typedef struct fcache_extent_s fcache_extent_t;

struct fcache_s {
    mutex_t lock;
    radix_tree_t tree;
    size_t num_pages;
    size_t num_dirty;
    bool memory;
    bool queued;
    vnode_t* dirty_next;
};

struct fcache_extent_s {
    uint64_t lba;
    size_t count;
    bool mapped;
};

// Called from within the fcache call the filesystem made,
// with whatever locks the filesystem took around that call
typedef int (*fcache_bmap_t)(vnode_t* restrict node, uint64_t sector, bool create, fcache_extent_t* restrict out);

void fcache_init(fcache_t* restrict cache);
void fcache_deinit(fcache_t* restrict cache);

/// Makes the cache the only copy of its pages
/// @pre The cache holds no page
/// @note The pages are never dirty and reclaim never takes them. The bmap of the calls is ignored,
/// and the filesystem needs no volume. A read of a hole gives zeroes and takes no page
void fcache_set_memory(fcache_t* restrict cache);

ssize_t fcache_read(vnode_t* restrict node, fcache_bmap_t bmap, void* restrict buffer, size_t count, off_t offset, off_t file_size);
ssize_t fcache_write(vnode_t* restrict node, fcache_bmap_t bmap, const void* restrict buffer, size_t count, off_t offset, off_t file_size);
int fcache_fault(vnode_t* restrict node, fcache_bmap_t bmap, off_t offset, off_t size, pmap_t* restrict pmap, uintptr_t virt,
    unsigned vprot);
int fcache_writeback(vnode_t* restrict node, fcache_bmap_t bmap, off_t size);
void fcache_truncate(vnode_t* restrict node, off_t size);
void fcache_forget(vnode_t* restrict node, uint64_t offset, uint64_t length);

int fcache_sync(vfs_t* restrict fs); // nullptr syncs all filesystems
size_t fcache_reclaim(size_t count);

vnode_t* fcache_dev_create(vfs_t* restrict fs) __nodiscard;
void fcache_dev_destroy(vnode_t* restrict device_node);
int fcache_dev_read(vnode_t* restrict device_node, uint64_t offset, void* restrict buffer, size_t size);
int fcache_dev_write(vnode_t* restrict device_node, uint64_t offset, const void* restrict buffer, size_t size);
