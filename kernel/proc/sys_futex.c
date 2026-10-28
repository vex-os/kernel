#include <vex/clocksrc.h>
#include <vex/errno.h>
#include <vex/futex.h>
#include <vex/process.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/uaccess.h>

static int check_flags(uintptr_t flags)
{
    if(flags & ~(uintptr_t)(FUTEX_SHARED)) {
        return -EINVAL;
    }

    return 0;
}

static uint64_t deadline_after(const timespec_t* restrict timeout)
{
    uint64_t now = clock_source_now_ns();
    uint64_t seconds = timeout->tv_sec;

    if(seconds >= (UINT64_MAX - now) / NSEC_PER_SEC) {
        return UINT64_MAX;
    }

    return now + seconds * NSEC_PER_SEC + timeout->tv_nsec;
}

static intptr_t sys_futex_wait(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    int retval = check_flags(args[3]);

    if(retval < 0) {
        return retval;
    }

    uint64_t deadline = 0;

    if(args[2]) {
        timespec_t timeout;
        ssize_t copied = user_copyin(&timeout, args[2], sizeof(timeout));

        if(copied < 0) {
            return copied;
        }

        if(timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= NSEC_PER_SEC) {
            return -EINVAL;
        }

        deadline = deadline_after(&timeout);
    }

    return futex_wait(process->vmspace, args[0], (uint32_t)(args[1]), deadline, args[3] & FUTEX_SHARED);
}

define_syscall(sys_futex_wait, SYS_FUTEX_WAIT);

static intptr_t sys_futex_wake(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    int retval = check_flags(args[2]);

    if(retval < 0) {
        return retval;
    }

    if(args[0] % sizeof(uint32_t)) {
        return -EINVAL;
    }

    return futex_wake(process->vmspace, args[0], args[1], args[2] & FUTEX_SHARED);
}

define_syscall(sys_futex_wake, SYS_FUTEX_WAKE);
