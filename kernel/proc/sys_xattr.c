#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/kmalloc.h>
#include <vex/path.h>
#include <vex/process.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vnode.h>
#include <vex/xattr.h>

#include "syscall_internal.h"

#define RESOLVE_FLAGS (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)

static int copyin_name(char* restrict name, uintptr_t user_name)
{
    ssize_t copied = user_copyin_str(name, user_name, XATTR_NAME_MAX + 1);

    if(copied == -ENAMETOOLONG) {
        return -ERANGE;
    }

    if(copied < 0) {
        return copied;
    }

    return 0;
}

static intptr_t sys_getxattrat(int dirfd, const char* restrict path, uintptr_t user_name, uintptr_t user_value, size_t size, unsigned flags)
{
    thread_t* thread = this_thread();
    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~RESOLVE_FLAGS) {
        return -EINVAL;
    }

    if(size > XATTR_SIZE_MAX) {
        size = XATTR_SIZE_MAX;
    }

    char name[XATTR_NAME_MAX + 1];
    int retval = copyin_name(name, user_name);

    if(retval < 0) {
        return retval;
    }

    vnode_t* node;
    retval = syscall_resolve_at(dirfd, path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    void* value = nullptr;

    if(size > 0) {
        value = kmalloc(size);
    }

    ssize_t length = -ENOMEM;

    if(size == 0 || value) {
        length = vnode_get_xattr(node, thread->credentials, name, value, size);
    }

    vnode_decref(node);

    if(length > 0 && size > 0) {
        ssize_t copied = user_copyout(user_value, value, length);

        if(copied < 0) {
            length = copied;
        }
    }

    if(value) {
        kfree(value);
    }

    return length;
}

define_syscall_dirfd(sys_getxattrat, SYS_GETXATTRAT, 6);

static intptr_t sys_setxattrat(int dirfd, const char* restrict path, uintptr_t user_name, uintptr_t user_value, size_t size, unsigned flags)
{
    thread_t* thread = this_thread();
    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(RESOLVE_FLAGS | XATTR_CREATE | XATTR_REPLACE)) {
        return -EINVAL;
    }

    if(size > XATTR_SIZE_MAX) {
        return -E2BIG;
    }

    char name[XATTR_NAME_MAX + 1];
    int retval = copyin_name(name, user_name);

    if(retval < 0) {
        return retval;
    }

    void* value = nullptr;

    if(size > 0) {
        value = kmalloc(size);

        if(value == nullptr) {
            return -ENOMEM;
        }

        ssize_t copied = user_copyin(value, user_value, size);

        if(copied < 0) {
            kfree(value);
            return copied;
        }
    }

    vnode_t* node;
    retval = syscall_resolve_at(dirfd, path, flags & RESOLVE_FLAGS, &node);

    if(retval == 0) {
        retval = vnode_set_xattr(node, thread->credentials, name, value, size, flags & (XATTR_CREATE | XATTR_REPLACE));
        vnode_decref(node);
    }

    if(value) {
        kfree(value);
    }

    return retval;
}

define_syscall_dirfd(sys_setxattrat, SYS_SETXATTRAT, 6);

static intptr_t sys_listxattrat(int dirfd, const char* restrict path, uintptr_t user_list, size_t size, unsigned flags)
{
    thread_t* thread = this_thread();
    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~RESOLVE_FLAGS) {
        return -EINVAL;
    }

    if(size > XATTR_LIST_MAX) {
        size = XATTR_LIST_MAX;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(dirfd, path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    char* list = nullptr;

    if(size > 0) {
        list = kmalloc(size);
    }

    ssize_t length = -ENOMEM;

    if(size == 0 || list) {
        length = vnode_list_xattr(node, thread->credentials, list, size);
    }

    vnode_decref(node);

    if(length > 0 && size > 0) {
        ssize_t copied = user_copyout(user_list, list, length);

        if(copied < 0) {
            length = copied;
        }
    }

    if(list) {
        kfree(list);
    }

    return length;
}

define_syscall_dirfd(sys_listxattrat, SYS_LISTXATTRAT, 5);

static intptr_t sys_removexattrat(int dirfd, const char* restrict path, uintptr_t user_name, unsigned flags)
{
    thread_t* thread = this_thread();
    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~RESOLVE_FLAGS) {
        return -EINVAL;
    }

    char name[XATTR_NAME_MAX + 1];
    int retval = copyin_name(name, user_name);

    if(retval < 0) {
        return retval;
    }

    vnode_t* node;
    retval = syscall_resolve_at(dirfd, path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_remove_xattr(node, thread->credentials, name);

    vnode_decref(node);

    return retval;
}

define_syscall_dirfd(sys_removexattrat, SYS_REMOVEXATTRAT, 4);
