#pragma once

#include <usr/vex/mount.h>
#include <vex/compiler.h>
#include <vex/errno.h>
#include <vex/statvfs.h>
#include <vex/types.h>

#define MNT_RDONLY __k_MNT_RDONLY

typedef struct cmdline_s cmdline_t;
typedef struct block_interface_s block_interface_t;
typedef struct dev_unit_s dev_unit_t;
typedef struct vnode_s vnode_t;

typedef struct vfs_s vfs_t;
typedef struct vfs_ops_s vfs_ops_t;
typedef struct vfs_driver_s vfs_driver_t;
typedef struct vfs_driver_private_s vfs_driver_private_t;

struct vfs_ops_s {
    /// Checks the device for this filesystem, for mounts that name no type
    /// @param fs nullptr, as nothing is mounted yet
    /// @retval 0 The device holds this filesystem
    int (*probe)(vfs_t* restrict fs, const block_interface_t* restrict volume);

    /// Sets root, holding a reference to it until unmount, and ctxdata
    /// @pre fs->device is set, and nullptr for a filesystem without one. fs->readonly is set if the mount asked for it;
    /// the driver may set it for a reason of its own, but must not clear it
    /// @post On failure nothing of the filesystem is left behind
    int (*mount)(vfs_t* restrict fs, const block_interface_t* restrict volume, const cmdline_t* restrict params);

    int (*statvfs)(vfs_t* restrict fs, statvfs_t* restrict out);

    /// @pre The vnode is fresh, with fs and ino set
    /// @retval -ENOENT The number no longer names a file
    int (*fill_node)(vfs_t* restrict fs, vnode_t* restrict node);

    /// Drops the root and frees what mount set up
    /// @pre The vcache is evicted, and no vnode but the root is referenced
    int (*unmount)(vfs_t* restrict fs);

    int (*sync)(vfs_t* restrict fs);
};

struct vfs_s {
    const vfs_ops_t* ops;
    vnode_t* mountpoint;
    vnode_t* root;
    const block_interface_t* volume; // Referenced for as long as the filesystem is mounted
    dev_unit_t* unit;                // Under dev_prefix_anon, for a filesystem without a device
    dev_t dev;                       // The st_dev of its files
    void* ctxdata;
    bool readonly;    // Set from the mount flags and parameters, and by the driver while mounting
    bool unmounting;  // Under mounts_lock; turns away operations crossing into the filesystem
    unsigned orphans; // Unlinked vnodes still referenced; they keep the filesystem busy
};

struct vfs_driver_private_s {
    vfs_driver_t* next;
};

struct vfs_driver_s {
    vfs_driver_private_t priv;
    const vfs_ops_t* restrict ops;
    const char* name;
    bool internal; // Only vfs_create makes it; vfs_mount turns it away
};

int vfs_driver_add(vfs_driver_t* restrict driver);
int vfs_driver_remove(vfs_driver_t* restrict driver);

/// Makes a filesystem that no path leads to
/// @param type The name of a driver. The filesystem has no device
/// @param params Options for the filesystem, or nullptr
/// @post On success out is mounted but not in the list of mounts, so nothing can unmount it
/// @retval -ENOENT No driver has this name
int vfs_create(const char* restrict type, const char* restrict params, vfs_t** restrict out);

/// @param params Options for the filesystem, or nullptr. The VFS itself takes "ro" and "readonly" from them, as MNT_RDONLY
/// @retval -ENODEV The driver is internal
int vfs_mount(const char* restrict type, const char* restrict path, const block_interface_t* restrict volume, unsigned flags,
    const char* restrict params);
int vfs_unmount(const char* restrict path);

int vfs_sync_fs(vfs_t* restrict fs);
int vfs_sync_all(void);
int vfs_statvfs(vfs_t* restrict fs, statvfs_t* restrict out);

vnode_t* vfs_get_root(void);
vnode_t* vfs_get_mountpoint(vfs_t* restrict fs);
vfs_t* vfs_get_fs(vnode_t* restrict node);
int vfs_cross_mount(vnode_t* restrict node, vnode_t** restrict out);
