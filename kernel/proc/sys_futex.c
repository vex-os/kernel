#include <vex/clocksrc.h>
#include <vex/errno.h>
#include <vex/futex.h>
#include <vex/process.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/uaccess.h>

#include "syscall_internal.h"

static int check_flags(uintptr_t flags)
{
    if(flags & ~(uintptr_t)(FUTEX_SHARED)) {
        return -EINVAL;
    }

    return 0;
}

static intptr_t sys_futex_wait(uintptr_t address, uint32_t value, uintptr_t user_timeout, uintptr_t flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    int retval = check_flags(flags);

    if(retval < 0) {
        return retval;
    }

    uint64_t deadline = 0;

    if(user_timeout) {
        timespec_t timeout;
        ssize_t copied = user_copyin(&timeout, user_timeout, sizeof(timeout));

        if(copied < 0) {
            return copied;
        }

        if(!timespec_valid(&timeout)) {
            return -EINVAL;
        }

        deadline = deadline_after_nsec(timespec_to_nsec(&timeout));
    }

    return futex_wait(process->vmspace, address, value, deadline, flags & FUTEX_SHARED);
}

define_syscall(sys_futex_wait, SYS_FUTEX_WAIT, 4);

static intptr_t sys_futex_wake(uintptr_t address, size_t count, uintptr_t flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    int retval = check_flags(flags);

    if(retval < 0) {
        return retval;
    }

    if(address % sizeof(uint32_t)) {
        return -EINVAL;
    }

    return futex_wake(process->vmspace, address, count, flags & FUTEX_SHARED);
}

define_syscall(sys_futex_wake, SYS_FUTEX_WAKE, 3);
