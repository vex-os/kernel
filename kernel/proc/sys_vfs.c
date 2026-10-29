#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/file.h>
#include <vex/mount.h>
#include <vex/path.h>
#include <vex/process.h>
#include <vex/statvfs.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vnode.h>

#include "syscall_internal.h"

static intptr_t sys_mount(user_addr_t source, user_addr_t target, user_addr_t type, uintptr_t flags, user_addr_t params)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    const credentials_t* credentials = thread->credentials;

    if(!cred_capable(credentials, CAP_MOUNT)) {
        return -EPERM;
    }

    if(flags & ~(uintptr_t)(MNT_RDONLY)) {
        return -EINVAL;
    }

    char mount_point[PATH_MAX];
    ssize_t copied = user_copyin_str(mount_point, target, sizeof(mount_point));

    if(copied < 0) {
        return copied;
    }

    char fstype[64];
    const char* fstype_ptr = nullptr;

    if(type.value) {
        copied = user_copyin_str(fstype, type, sizeof(fstype));

        if(copied < 0) {
            return copied;
        }

        fstype_ptr = fstype;
    }

    char options[256];
    const char* params_ptr = nullptr;

    if(params.value) {
        copied = user_copyin_str(options, params, sizeof(options));

        if(copied < 0) {
            return copied;
        }

        params_ptr = options;
    }

    const block_interface_t* interface = nullptr;

    if(source.value) {
        char device_path[PATH_MAX];
        copied = user_copyin_str(device_path, source, sizeof(device_path));

        if(copied < 0) {
            return copied;
        }

        if(device_path[0]) {
            vnode_t* base_dir;
            int retval = syscall_dirfd(AT_FDCWD, device_path, &base_dir);

            if(retval < 0) {
                return retval;
            }

            vnode_t* source_node;
            retval = path_resolve_at(base_dir, device_path, 0, credentials, &source_node);

            if(base_dir) {
                vnode_decref(base_dir);
            }

            if(retval < 0) {
                return retval;
            }

            if(source_node->type != VNODE_BLOCK_DEV || source_node->volume == nullptr) {
                vnode_decref(source_node);
                return -EINVAL;
            }

            interface = source_node->volume;
            block_interface_incref(interface);
            vnode_decref(source_node);
        }
    }

    // Only a device lets the VFS find the type by itself
    if(fstype_ptr == nullptr && interface == nullptr) {
        return -EINVAL;
    }

    int retval = vfs_mount(fstype_ptr, mount_point, interface, flags, params_ptr);

    if(interface) {
        block_interface_decref(interface);
    }

    return retval;
}

define_syscall(sys_mount, SYS_MOUNT, 5);

static intptr_t sys_umount(user_addr_t path)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    const credentials_t* credentials = thread->credentials;

    if(!cred_capable(credentials, CAP_MOUNT)) {
        return -EPERM;
    }

    char mount_point[PATH_MAX];
    ssize_t copied = user_copyin_str(mount_point, path, sizeof(mount_point));

    if(copied < 0) {
        return copied;
    }

    return vfs_unmount(mount_point);
}

define_syscall(sys_umount, SYS_UMOUNT, 1);

static intptr_t sys_sync(void)
{
    return vfs_sync_all();
}

define_syscall(sys_sync, SYS_SYNC, 0);

static intptr_t sys_syncfs(file_t* restrict file)
{
    vnode_t* node;
    int retval = vfs_cross_mount(file->node, &node);

    if(retval < 0) {
        return retval;
    }

    vfs_t* fs = node->fs;

    if(fs == nullptr) {
        retval = -EINVAL;
    }
    else {
        retval = vfs_sync_fs(fs);
    }

    vnode_decref(node);

    return retval;
}

define_syscall_fd(sys_syncfs, SYS_SYNCFS, 1);

static intptr_t sys_statvfs(file_t* restrict file, user_addr_t buffer)
{
    vnode_t* node;
    int retval = vfs_cross_mount(file->node, &node);

    if(retval < 0) {
        return retval;
    }

    vfs_t* fs = node->fs;
    statvfs_t st;

    if(fs == nullptr) {
        retval = -EINVAL;
    }
    else {
        retval = vfs_statvfs(fs, &st);
    }

    vnode_decref(node);

    if(retval < 0) {
        return retval;
    }

    ssize_t copied = user_copyout(buffer, &st, sizeof(st));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall_fd(sys_statvfs, SYS_STATVFS, 2);
