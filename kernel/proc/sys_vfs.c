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

static intptr_t sys_mount(uintptr_t user_source, uintptr_t user_target, uintptr_t user_type, uintptr_t flags, uintptr_t user_params)
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

    char target[PATH_MAX];
    ssize_t copied = user_copyin_str(target, user_target, sizeof(target));

    if(copied < 0) {
        return copied;
    }

    char fstype[64];
    const char* fstype_ptr = nullptr;

    if(user_type) {
        copied = user_copyin_str(fstype, user_type, sizeof(fstype));

        if(copied < 0) {
            return copied;
        }

        fstype_ptr = fstype;
    }

    char params[256];
    const char* params_ptr = nullptr;

    if(user_params) {
        copied = user_copyin_str(params, user_params, sizeof(params));

        if(copied < 0) {
            return copied;
        }

        params_ptr = params;
    }

    const block_interface_t* interface = nullptr;

    if(user_source) {
        char source[PATH_MAX];
        copied = user_copyin_str(source, user_source, sizeof(source));

        if(copied < 0) {
            return copied;
        }

        if(source[0]) {
            vnode_t* base_dir;
            int retval = syscall_dirfd(AT_FDCWD, source, &base_dir);

            if(retval < 0) {
                return retval;
            }

            vnode_t* source_node;
            retval = path_resolve_at(base_dir, source, 0, credentials, &source_node);

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

    int retval = vfs_mount(fstype_ptr, target, interface, flags, params_ptr);

    if(interface) {
        block_interface_decref(interface);
    }

    return retval;
}

define_syscall(sys_mount, SYS_MOUNT, 5);

static intptr_t sys_umount(uintptr_t user_path)
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

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, user_path, sizeof(path));

    if(copied < 0) {
        return copied;
    }

    return vfs_unmount(path);
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

static intptr_t sys_statvfs(file_t* restrict file, uintptr_t buffer)
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
