#include <vex/capability.h>
#include <vex/clocksrc.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/uaccess.h>
#include <vex/wallclock.h>

static intptr_t sys_gettime(const uintptr_t* restrict args)
{
    timespec_t time;

    switch(args[0]) {
        case CLOCK_REALTIME:
            wallclock_get(&time);
            break;

        case CLOCK_MONOTONIC:
            timespec_from_nsec(&time, clock_source_now_ns());
            break;

        default:
            return -EINVAL;
    }

    ssize_t copied = user_copyout(args[1], &time, sizeof(time));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_gettime, SYS_GETTIME);

static intptr_t sys_settime(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(args[0] != CLOCK_REALTIME) {
        return -EINVAL;
    }

    timespec_t time;
    ssize_t copied = user_copyin(&time, args[1], sizeof(time));

    if(copied < 0) {
        return copied;
    }

    if(!cred_capable(thread->credentials, CAP_SYS_TIME)) {
        return -EPERM;
    }

    return wallclock_set(&time);
}

define_syscall(sys_settime, SYS_SETTIME);

// Nobody triggers the event, so only the timeout or a signal ends the wait
static ssize_t sleep_nanoseconds(uint64_t timeout)
{
    event_t event;
    event_init(&event);

    event_t* event_pointer = &event;
    spinlock_t lock = SPINLOCK_INIT;
    bool intr_state = spinlock_acquire_intr(&lock);
    ssize_t result = event_await_unlock(&event_pointer, 1, &lock, intr_state, timeout);

    event_deinit(&event);

    if(result == -ETIMEDOUT) {
        return 0;
    }

    return result;
}

static ssize_t sleep_relative(uint64_t timeout, uintptr_t remaining_address)
{
    uint64_t start = clock_source_now_ns();
    ssize_t result = sleep_nanoseconds(timeout);

    if(result != -ERESTART) {
        return result;
    }

    if(remaining_address) {
        uint64_t elapsed = clock_source_now_ns() - start;
        timespec_t remaining = {};

        if(elapsed < timeout) {
            timespec_from_nsec(&remaining, timeout - elapsed);
        }

        ssize_t copied = user_copyout(remaining_address, &remaining, sizeof(remaining));

        if(copied < 0) {
            return copied;
        }
    }

    return -EINTR;
}

static ssize_t sleep_until_monotonic(uint64_t target)
{
    uint64_t now = clock_source_now_ns();

    if(target <= now) {
        return 0;
    }

    ssize_t result = sleep_nanoseconds(target - now);

    if(result == -ERESTART) {
        return -EINTR;
    }

    return result;
}

static ssize_t sleep_until_realtime(const timespec_t* restrict target)
{
    ssize_t result = wallclock_wait_until(target);

    if(result == -ERESTART) {
        return -EINTR;
    }

    return result;
}

// A signal ends the sleep for good, as POSIX has it, so the call never restarts
static intptr_t sys_nanosleep(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(args[0] != CLOCK_REALTIME && args[0] != CLOCK_MONOTONIC) {
        return -EINVAL;
    }

    if(0 != (args[1] & ~(uintptr_t)(TIMER_ABSTIME))) {
        return -EINVAL;
    }

    timespec_t request;
    ssize_t copied = user_copyin(&request, args[2], sizeof(request));

    if(copied < 0) {
        return copied;
    }

    if(!timespec_valid(&request)) {
        return -EINVAL;
    }

    if(args[1] & TIMER_ABSTIME) {
        if(args[0] == CLOCK_REALTIME) {
            return sleep_until_realtime(&request);
        }

        return sleep_until_monotonic(timespec_to_nsec(&request));
    }

    uint64_t timeout = timespec_to_nsec(&request);

    // Zero would mean to wait for as long as it takes
    if(timeout == 0) {
        sched_yield();
        return 0;
    }

    return sleep_relative(timeout, args[3]);
}

define_syscall(sys_nanosleep, SYS_NANOSLEEP);
