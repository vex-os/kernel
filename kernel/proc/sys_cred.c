#include <vex/atomic.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/process.h>
#include <vex/stat.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>

#include "syscall_internal.h"

static int copyout_res(uintptr_t user_r, uintptr_t user_e, uintptr_t user_s, int32_t r, int32_t e, int32_t s)
{
    ssize_t copied = user_copyout(user_r, &r, sizeof(r));

    if(copied >= 0) {
        copied = user_copyout(user_e, &e, sizeof(e));
    }

    if(copied >= 0) {
        copied = user_copyout(user_s, &s, sizeof(s));
    }

    if(copied < 0) {
        return copied;
    }

    return 0;
}

static intptr_t sys_getresuid(uintptr_t user_ruid, uintptr_t user_euid, uintptr_t user_suid)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    const credentials_t* credentials = thread->credentials;

    return copyout_res(user_ruid, user_euid, user_suid, credentials->uid, credentials->euid, credentials->suid);
}

define_syscall(sys_getresuid, SYS_GETRESUID, 3);

static intptr_t sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    mutex_acquire(&process->credentials_lock);

    credentials_t* credentials = cred_copy(process->credentials);
    int retval = -ENOMEM;

    if(credentials) {
        retval = cred_setresuid(credentials, ruid, euid, suid);
    }

    if(retval == 0) {
        process_set_cred(process, credentials);
    }
    else if(credentials) {
        cred_decref(credentials);
    }

    mutex_release(&process->credentials_lock);

    return retval;
}

define_syscall(sys_setresuid, SYS_SETRESUID, 3);

static intptr_t sys_getresgid(uintptr_t user_rgid, uintptr_t user_egid, uintptr_t user_sgid)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    const credentials_t* credentials = thread->credentials;

    return copyout_res(user_rgid, user_egid, user_sgid, credentials->gid, credentials->egid, credentials->sgid);
}

define_syscall(sys_getresgid, SYS_GETRESGID, 3);

static intptr_t sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    mutex_acquire(&process->credentials_lock);

    credentials_t* credentials = cred_copy(process->credentials);
    int retval = -ENOMEM;

    if(credentials) {
        retval = cred_setresgid(credentials, rgid, egid, sgid);
    }

    if(retval == 0) {
        process_set_cred(process, credentials);
    }
    else if(credentials) {
        cred_decref(credentials);
    }

    mutex_release(&process->credentials_lock);

    return retval;
}

define_syscall(sys_setresgid, SYS_SETRESGID, 3);

static intptr_t sys_getgroups(size_t size, uintptr_t user_list)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    const credentials_t* credentials = thread->credentials;
    if(size == 0) {
        return credentials->num_groups;
    }

    if(size < credentials->num_groups) {
        return -EINVAL;
    }

    ssize_t copied = user_copyout(user_list, credentials->groups, credentials->num_groups * sizeof(gid_t));

    if(copied < 0) {
        return copied;
    }

    return credentials->num_groups;
}

define_syscall(sys_getgroups, SYS_GETGROUPS, 2);

static intptr_t sys_setgroups(size_t size, uintptr_t user_list)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(size > atomic_load_n(&cred_max_groups, ATOMIC_RELAXED)) {
        return -EINVAL;
    }

    mutex_acquire(&process->credentials_lock);

    if(!cred_capable(process->credentials, CAP_SETGID)) {
        mutex_release(&process->credentials_lock);
        return -EPERM;
    }

    credentials_t* credentials = cred_copy_ids(process->credentials, size);
    int retval = -ENOMEM;

    if(credentials) {
        ssize_t copied = user_copyin(credentials->groups, user_list, size * sizeof(gid_t));

        if(copied < 0) {
            retval = copied;
        }
        else {
            retval = cred_sort_groups(credentials);
        }
    }

    if(retval == 0) {
        process_set_cred(process, credentials);
    }
    else if(credentials) {
        cred_decref(credentials);
    }

    mutex_release(&process->credentials_lock);

    return retval;
}

define_syscall(sys_setgroups, SYS_SETGROUPS, 2);

static intptr_t sys_getumask(void)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    return atomic_load_n(&process->umask, ATOMIC_RELAXED);
}

define_syscall(sys_getumask, SYS_GETUMASK, 0);

static intptr_t sys_setumask(mode_t mask)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    return atomic_exchange(&process->umask, mask & (S_IRWXU | S_IRWXG | S_IRWXO), ATOMIC_RELAXED);
}

define_syscall(sys_setumask, SYS_SETUMASK, 1);

static intptr_t sys_capget(uintptr_t user_caps)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    const credentials_t* credentials = thread->credentials;

    caps_t caps = {};
    caps.effective = credentials->cap_effective;
    caps.permitted = credentials->cap_permitted;
    caps.ambient = credentials->cap_ambient;
    caps.bounding = credentials->cap_bounding;
    caps.flags = credentials->cap_flags;

    ssize_t copied = user_copyout(user_caps, &caps, sizeof(caps));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_capget, SYS_CAPGET, 1);

static intptr_t sys_capset(uintptr_t user_caps)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    caps_t caps;
    ssize_t copied = user_copyin(&caps, user_caps, sizeof(caps));

    if(copied < 0) {
        return copied;
    }

    mutex_acquire(&process->credentials_lock);

    credentials_t* credentials = cred_copy(process->credentials);
    int retval = -ENOMEM;

    if(credentials) {
        retval = cred_setcaps(credentials, &caps);
    }

    if(retval == 0) {
        process_set_cred(process, credentials);
    }
    else if(credentials) {
        cred_decref(credentials);
    }

    mutex_release(&process->credentials_lock);

    return retval;
}

define_syscall(sys_capset, SYS_CAPSET, 1);
