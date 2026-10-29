#pragma once

#include <vex/compiler.h>
#include <vex/mutex.h>
#include <vex/time.h>
#include <vex/types.h>

typedef struct vfs_s vfs_t;
typedef struct vnode_s vnode_t;
typedef struct vnode_ops_s vnode_ops_t;

#define TMPFS_ROOT_INO 1

typedef struct tmpfs_s tmpfs_t;
typedef struct tmpfs_inode_s tmpfs_inode_t;
typedef struct tmpfs_entry_s tmpfs_entry_t;

struct tmpfs_s {
    mutex_t lock; // Guards the counters, the list of inodes, every inode and every directory
    tmpfs_inode_t* inodes;
    ino_t next_ino;
    size_t max_pages; // Of file contents; SIZE_MAX when the mount has no limit
    size_t max_nodes; // SIZE_MAX when the mount has no limit
    size_t used_pages;
    size_t used_nodes;
};

// A node of the filesystem has one reference of its own for as long as it has a name
struct tmpfs_inode_s {
    tmpfs_inode_t* all_prev;
    tmpfs_inode_t* all_next;
    tmpfs_t* tmpfs;
    vnode_t* node;
    mode_t mode; // The permission bits only; the type is the one of the vnode
    uid_t uid;
    gid_t gid;
    unsigned nlink;
    off_t size;   // For a symlink, the length of the target
    char* target; // Of a symlink; no terminator. A slab object up to SLAB_MAX_SIZE bytes long, a pslab page above that
    timespec_t atime;
    timespec_t mtime;
    timespec_t ctime;
    tmpfs_entry_t* first_entry;
    tmpfs_entry_t* last_entry;
    uint64_t next_seq;
};

struct tmpfs_entry_s {
    tmpfs_entry_t* prev;
    tmpfs_entry_t* next;
    vnode_t* node;
    uint64_t seq; // The position in the directory; grows and never repeats
    char name[];
};

extern const vnode_ops_t tmpfs_vnode_ops;

/// Takes pages of file contents from the limit of the mount and from the commit limit
/// @pre The caller holds the lock of the filesystem
/// @retval -ENOSPC One of the two limits has no room; nothing is taken
int tmpfs_charge(tmpfs_t* restrict tmpfs, size_t pages);

/// Gives back pages that tmpfs_charge took
/// @pre The caller holds the lock of the filesystem
void tmpfs_uncharge(tmpfs_t* restrict tmpfs, size_t pages);

/// Makes a node with one reference, the one of the filesystem
/// @pre The caller holds the lock of the filesystem, or the filesystem is mounting
/// @pre fs->ctxdata is set
/// @retval -ENOSPC The mount has its largest number of nodes
int tmpfs_node_alloc(vfs_t* restrict fs, unsigned type, mode_t mode, uid_t uid, gid_t gid, vnode_t** restrict out);
