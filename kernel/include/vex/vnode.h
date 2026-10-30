#pragma once

#include <vex/compiler.h>
#include <vex/cred.h>
#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/spinlock.h>
#include <vex/stat.h>
#include <vex/time.h>
#include <vex/types.h>
#include <vex/xattr.h>

typedef struct file_s file_t;
typedef struct file_locks_s file_locks_t;
typedef struct pmap_s pmap_t;
typedef struct poll_table_s poll_table_t;
typedef struct vcache_entry_s vcache_entry_t;
typedef struct block_interface_s block_interface_t;
typedef struct vfs_s vfs_t;

enum {
    VNODE_FILE,
    VNODE_DIRECTORY,
    VNODE_BLOCK_DEV,
    VNODE_CHAR_DEV,
    VNODE_SYM_LINK,
    VNODE_FIFO,
};

enum {
    VNODE_CAPS_UNKNOWN,
    VNODE_CAPS_NONE,
    VNODE_CAPS_PRESENT,
};

#define VATTR_MODE  0x0001U
#define VATTR_UID   0x0002U
#define VATTR_GID   0x0004U
#define VATTR_SIZE  0x0008U
#define VATTR_ATIME 0x0010U
#define VATTR_MTIME 0x0020U
#define VATTR_CTIME 0x0040U

typedef struct vnode_attr_s vnode_attr_t;
typedef struct vnode_s vnode_t;
typedef struct vnode_ops_s vnode_ops_t;

struct vnode_attr_s {
    unsigned mask; // VATTR; fields to change, the others are ignored
    mode_t mode;
    uid_t uid;
    gid_t gid;
    off_t size;
    timespec_t atime;
    timespec_t mtime;
    timespec_t ctime;
};

/// Implemented by each filesystem. If an operation is nullptr, the VFS fails the call
/// Before an operation runs, the VFS crosses mount points and rejects these cases:
/// "." and ".." as new names, links or renames across filesystems, and changes to a read-only filesystem
/// The filesystem serializes its operations with its own lock
struct vnode_ops_s {
    /// @note Also frees the storage of a file whose last name was removed while it was open
    void (*release)(vnode_t* restrict node);

    /// Finds only the inode number; the VFS gets the vnode from the vcache or through vfs_ops_t.fill_node
    int (*lookup)(vnode_t* restrict node, const char* restrict name, ino_t* restrict out);

    /// Emits the entries from dir_context->position on, until the end of the directory or until dir_emit fails
    /// @pre node is a directory
    /// @param dir_context Its position is a cookie that only this operation reads; 0 is the start
    /// @post dir_context->position is the cookie of the first entry that was not emitted
    /// @note A cookie must keep working after an unlink: the iteration goes on with the next entry
    int (*iterate)(vnode_t* restrict node, dir_context_t* restrict dir_context);

    /// @post On success out has one reference and fs and ino set, and isn't in the vcache: the VFS inserts it
    /// @note A filesystem that keeps all its nodes cached may vcache_insert_pinned the node itself. It then also sets
    /// the parent of a directory and holds a reference to it. The VFS keeps the node and adds the reference of the caller
    int (*create)(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, const credentials_t* restrict credentials,
        vnode_t** restrict out);

    /// @param target The vnode the name led to
    /// @note Once its last name is gone, vcache_remove the target under the filesystem's lock, before the entry
    /// can be reused, or a new file there would get this vnode
    int (*unlink)(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target);

    /// @param victim The vnode newname led to, or nullptr
    /// @pre The VFS has filtered out renaming a file onto itself and a directory into its own subtree
    /// @note The victim is unlinked as unlink does it. If the inode number depends on the location of the entry,
    /// call vcache_rekey for the source under the same lock
    int (*rename)(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim);

    /// Makes a FIFO or a device node; nullptr makes mknod fail with EPERM
    /// @param type VNODE_FIFO, VNODE_CHAR_DEV or VNODE_BLOCK_DEV
    /// @param rdev The device that a device node stands for; zero for a FIFO
    /// @post As for create, and the filesystem has called vnode_init_special on out, and does so on every vnode that it
    /// makes for the node later
    int (*mknod)(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, dev_t rdev,
        const credentials_t* restrict credentials, vnode_t** restrict out);

    /// @pre The target isn't a directory
    int (*link)(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target);

    /// @post On success out has one reference and fs and ino set, and isn't in the vcache: the VFS inserts it
    int (*symlink)(vnode_t* restrict node, const char* restrict name, const char* restrict target,
        const credentials_t* restrict credentials, vnode_t** restrict out);

    /// @pre file has its node, flags and offset set, and a nullptr ctxdata
    /// @note Runs for each open; file->ctxdata is the node's to set,
    /// and close gets the file once its last reference is gone
    int (*open)(vnode_t* restrict node, file_t* restrict file);

    int (*close)(vnode_t* restrict node, file_t* restrict file);
    ssize_t (*read)(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset);
    ssize_t (*write)(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset);

    /// Moves the file's offset by the node's own rules, for files whose offset isn't a byte position
    /// @return The new offset
    /// @retval -ENOSYS The VFS moves the offset as a byte position
    off_t (*seek)(vnode_t* restrict node, file_t* restrict file, off_t offset, int origin);

    ssize_t (*readlink)(vnode_t* restrict node, char* restrict buf, size_t size);
    int (*ioctl)(vnode_t* restrict node, file_t* restrict file, uint32_t request, uintptr_t argument);

    /// Reports the state of the file as POLL bits, without a wait
    /// @param table Gets, through poll_table_add, each event whose trigger can change the result
    /// @note nullptr makes the file always ready to read and write
    unsigned (*poll)(vnode_t* restrict node, file_t* restrict file, poll_table_t* restrict table);

    int (*stat)(vnode_t* restrict node, stat_t* restrict out);
    int (*fsync)(vnode_t* restrict node);

    /// Makes sure that the storage of the range exists, so that a write into it does not fail for lack of space
    /// @pre The file is a regular one, offset is not negative, length is positive, offset + length does not overflow,
    /// and mode has no bits but FALLOC_FL_KEEP_SIZE
    /// @param mode 0 also makes the file longer if the range ends past its end. KEEP_SIZE leaves the size alone
    /// @retval -EOPNOTSUPP The filesystem cannot do this mode
    /// @retval -ENOSPC The storage ran out. What was made stays, and so does the size
    /// @post What was newly made reads as zeroes. The mtime and the ctime move if anything changed
    int (*fallocate)(vnode_t* restrict node, unsigned mode, off_t offset, off_t length);

    /// @pre Permission checks are done, and the VFS has set the ctime, and the mtime for a size change,
    /// unless the caller did
    int (*set_attr)(vnode_t* restrict node, const vnode_attr_t* restrict attr);

    /// Maps the file's contents at offset at virt, normally through fcache_fault
    /// @pre offset is page aligned
    /// @retval VMS_FAULT_MINOR The page was in the cache
    /// @retval VMS_FAULT_MAJOR The page had to be read in first
    int (*fault)(vnode_t* restrict node, off_t offset, pmap_t* restrict pmap, virt_addr_t virt, unsigned vprot);

    /// Checks a mapping before mmap makes it; nullptr allows any
    /// @pre offset and length are page aligned
    /// @retval -ENXIO The range runs past the end of the memory of the node
    int (*mmap)(vnode_t* restrict node, off_t offset, size_t length);

    /// Gives the physical address and the memory type (a VPROT_CACHE_* value) of the page at offset
    /// @pre offset is page aligned. The caller holds a spinlock, so this must not sleep
    /// @retval -ENXIO offset is past the end of the memory of the node
    /// @note Set only by device nodes, whose memory is mapped through it instead of fault
    int (*resolve_page)(vnode_t* restrict node, off_t offset, phys_addr_t* restrict phys, unsigned* restrict cache);

    /// @param space The name's namespace, as an XATTR space
    /// @param name Without its namespace prefix
    /// @pre Permission checks are done
    /// @retval -ENODATA No such attribute
    ssize_t (*get_xattr)(vnode_t* restrict node, unsigned space, const char* restrict name, void* restrict value, size_t size);

    /// @param space The name's namespace, as an XATTR space
    /// @param name Without its namespace prefix
    /// @pre Permission checks are done
    int (*set_xattr)(vnode_t* restrict node, unsigned space, const char* restrict name, const void* restrict value, size_t size,
        unsigned flags);

    /// @retval -ENOENT index is past the last attribute
    /// @note Counts every namespace, as the VFS hides what the caller may not see
    int (*iterate_xattr)(vnode_t* restrict node, xattr_entry_t* restrict out, size_t index);

    /// @param space The name's namespace, as an XATTR space
    /// @param name Without its namespace prefix
    /// @pre Permission checks are done
    /// @retval -ENODATA No such attribute
    int (*remove_xattr)(vnode_t* restrict node, unsigned space, const char* restrict name);
};

struct vnode_s {
    vfs_t* fs;
    vfs_t* mounted_fs;
    vnode_t* parent;
    const vnode_ops_t* ops;
    spinlock_t lock;
    unsigned refcount;
    unsigned type;
    bool orphaned;
    unsigned privs_seq;
    unsigned caps_state;
    bool privs_clear;
    ino_t ino;
    vcache_entry_t* cache_entry;
    const block_interface_t* volume;
    fcache_t pages;
    file_locks_t* locks;            // Made on the first lock, and kept until the node goes
    const vnode_ops_t* special_ops; // The I/O of a FIFO or a device node, which is not the one of its filesystem
    void* special_data;             // What special_ops keep: the pipe of a FIFO while it is open
    dev_t rdev;                     // The device of a device node
    void* ctxdata;
};

vnode_t* vnode_alloc(unsigned type, const vnode_ops_t* restrict ops, void* restrict ctxdata) __nodiscard;
void vnode_incref(vnode_t* restrict node);
bool vnode_decref(vnode_t* restrict node);
void vnode_destroy(vnode_t* restrict node);

int vnode_lookup(vnode_t* restrict node, const char* restrict name, vnode_t** restrict out);
int vnode_iterate(vnode_t* restrict node, dir_context_t* restrict dir_context);
/// Gives a FIFO or a device node of a filesystem the I/O of its kind: open, close, read, write, seek, ioctl and poll
/// go to it, and the others stay with the filesystem
/// @pre The node is new, and nothing has opened it
void vnode_init_special(vnode_t* restrict node, dev_t rdev);

int vnode_mknod(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, dev_t rdev,
    const credentials_t* restrict credentials, vnode_t** restrict out);
int vnode_create(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, const credentials_t* restrict credentials,
    vnode_t** restrict out);
int vnode_unlink(vnode_t* restrict node, const char* restrict name, bool directory);
int vnode_rename(vnode_t* olddir, const char* oldname, vnode_t* newdir, const char* newname);
int vnode_link(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target);
int vnode_symlink(vnode_t* restrict node, const char* restrict name, const char* restrict target, const credentials_t* restrict credentials,
    vnode_t** restrict out);
int vnode_open(vnode_t* restrict node, file_t* restrict file);
int vnode_close(vnode_t* restrict node, file_t* restrict file);
ssize_t vnode_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset);
ssize_t vnode_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset);
off_t vnode_seek(vnode_t* restrict node, file_t* restrict file, off_t offset, int origin);
ssize_t vnode_readlink(vnode_t* restrict node, char* restrict buf, size_t size);
int vnode_stat(vnode_t* restrict node, stat_t* restrict out);

/// @note Every change is a status change, so the ctime moves too, unless the caller set it. A new size also changes the mtime
int vnode_set_attr(vnode_t* restrict node, const vnode_attr_t* restrict request);

int vnode_chmod(vnode_t* restrict node, const credentials_t* restrict credentials, mode_t mode);
int vnode_chown(vnode_t* restrict node, const credentials_t* restrict credentials, uid_t uid, gid_t gid);
int vnode_utimes(vnode_t* restrict node, const credentials_t* restrict credentials, const timespec_t* restrict times);
int vnode_may_delete(vnode_t* restrict dir, const char* restrict name, const credentials_t* restrict credentials) __nodiscard;
int vnode_access(vnode_t* restrict node, const credentials_t* restrict credentials, unsigned mask) __nodiscard;
int vnode_remove_privs(vnode_t* restrict node, const credentials_t* restrict credentials) __nodiscard;
int vnode_ioctl(vnode_t* restrict node, file_t* restrict file, uint32_t request, uintptr_t argument);
unsigned vnode_poll(vnode_t* restrict node, file_t* restrict file, poll_table_t* restrict table);
int vnode_fsync(vnode_t* restrict node);

/// @retval -EOPNOTSUPP The filesystem has no fallocate operation
int vnode_fallocate(vnode_t* restrict node, unsigned mode, off_t offset, off_t length);

/// @retval VMS_FAULT_MINOR The page was in the cache
/// @retval VMS_FAULT_MAJOR The page had to be read in first
int vnode_fault(vnode_t* restrict node, off_t offset, pmap_t* restrict pmap, virt_addr_t virt, unsigned vprot);
int vnode_mmap(vnode_t* restrict node, off_t offset, size_t length);
int vnode_resolve_page(vnode_t* restrict node, off_t offset, phys_addr_t* restrict phys, unsigned* restrict cache);
ssize_t vnode_get_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, const char* restrict name, void* restrict value,
    size_t size);
int vnode_set_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, const char* restrict name,
    const void* restrict value, size_t size, unsigned flags);
ssize_t vnode_list_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, char* restrict list, size_t size);
int vnode_remove_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, const char* restrict name);

/// @note Once a node is known to have none, this skips the permission check and the call into the filesystem
ssize_t vnode_get_caps(vnode_t* restrict node, void* restrict value, size_t size);
