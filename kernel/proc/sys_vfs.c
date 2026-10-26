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

static intptr_t syncfs_file(file_t* restrict file, const uintptr_t* restrict args)
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

static intptr_t statvfs_file(file_t* restrict file, const uintptr_t* restrict args)
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

    ssize_t copied = user_copyout(args[1], &st, sizeof(st));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

static intptr_t sys_mount(const uintptr_t* restrict args)
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

    uintptr_t flags = args[3];

    if(flags & ~(uintptr_t)(MNT_RDONLY)) {
        return -EINVAL;
    }

    char target[PATH_MAX];
    ssize_t copied = user_copyin_str(target, args[1], sizeof(target));

    if(copied < 0) {
        return copied;
    }

    char fstype[64];
    const char* fstype_ptr = nullptr;

    if(args[2]) {
        copied = user_copyin_str(fstype, args[2], sizeof(fstype));

        if(copied < 0) {
            return copied;
        }

        fstype_ptr = fstype;
    }

    char params[256];
    const char* params_ptr = nullptr;

    if(args[4]) {
        copied = user_copyin_str(params, args[4], sizeof(params));

        if(copied < 0) {
            return copied;
        }

        params_ptr = params;
    }

    const block_interface_t* interface = nullptr;

    if(args[0]) {
        char source[PATH_MAX];
        copied = user_copyin_str(source, args[0], sizeof(source));

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

define_syscall(sys_mount, SYS_MOUNT);

static intptr_t sys_umount(const uintptr_t* restrict args)
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
    ssize_t copied = user_copyin_str(path, args[0], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    return vfs_unmount(path);
}

define_syscall(sys_umount, SYS_UMOUNT);

static intptr_t sys_sync(const uintptr_t* restrict args)
{
    return vfs_sync_all();
}

define_syscall(sys_sync, SYS_SYNC);

static intptr_t sys_syncfs(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &syncfs_file);
}

define_syscall(sys_syncfs, SYS_SYNCFS);

static intptr_t sys_statvfs(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &statvfs_file);
}

define_syscall(sys_statvfs, SYS_STATVFS);
