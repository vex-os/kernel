#include <vex/capability.h>
#include <vex/cputime.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/process.h>
#include <vex/resource.h>
#include <vex/rlimit.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>

#include "syscall_internal.h"

static intptr_t sys_getrlimit(uintptr_t resource, user_addr_t out)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(resource >= RLIMIT_NLIMITS) {
        return -EINVAL;
    }

    rlimit_t limit = process_get_rlimit(process, resource);
    ssize_t copied = user_copyout(out, &limit, sizeof(limit));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_getrlimit, SYS_GETRLIMIT, 2);

static intptr_t sys_setrlimit(uintptr_t resource, user_addr_t in)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(resource >= RLIMIT_NLIMITS) {
        return -EINVAL;
    }

    rlimit_t limit;
    ssize_t copied = user_copyin(&limit, in, sizeof(limit));

    if(copied < 0) {
        return copied;
    }

    bool privileged = cred_capable(thread->credentials, CAP_SYS_RESOURCE);

    return process_set_rlimit(process, resource, &limit, privileged);
}

define_syscall(sys_setrlimit, SYS_SETRLIMIT, 2);

// A child counts once wait collects it, with the children that it collected itself
static void children_cputime(process_t* restrict process, cputime_t* restrict out)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);
    *out = process->children_cputime;
    spinlock_release_intr(&process->lock, intr_state);
}

static intptr_t sys_getrusage(intptr_t who, user_addr_t out)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    cputime_t time = {};

    switch(who) {
        case RUSAGE_SELF:
            cputime_of_process(process, &time);
            break;

        case RUSAGE_CHILDREN:
            children_cputime(process, &time);
            break;

        case RUSAGE_THREAD:
            cputime_of_thread(thread, &time);
            break;

        default:
            return -EINVAL;
    }

    rusage_t usage;
    cputime_to_rusage(&time, &usage);

    ssize_t copied = user_copyout(out, &usage, sizeof(usage));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_getrusage, SYS_GETRUSAGE, 2);
