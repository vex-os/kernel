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

#include "syscall_internal.h"

// The kernel steps the realtime clock and never slews, so the raw and coarse clocks read as the fine ones
// TODO: CLOCK_BOOTTIME is not an alias once suspend exists, as it counts the time spent suspended
// UNDONE: CLOCK_PROCESS_CPUTIME_ID and CLOCK_THREAD_CPUTIME_ID need the CPU time counters (RDTSC?)
static int canonical_clock(uintptr_t clock)
{
    switch(clock) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
            return CLOCK_REALTIME;

        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
            return CLOCK_MONOTONIC;

        default:
            return -EINVAL;
    }
}

static intptr_t sys_gettime(uintptr_t clock, user_addr_t time)
{
    timespec_t now;

    switch(canonical_clock(clock)) {
        case CLOCK_REALTIME:
            wallclock_get(&now);
            break;

        case CLOCK_MONOTONIC:
            timespec_from_nsec(&now, clock_source_now_ns());
            break;

        default:
            return -EINVAL;
    }

    ssize_t copied = user_copyout(time, &now, sizeof(now));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_gettime, SYS_GETTIME, 2);

static intptr_t sys_settime(uintptr_t clock, user_addr_t time)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    if(clock != CLOCK_REALTIME) {
        return -EINVAL;
    }

    timespec_t new_time;
    ssize_t copied = user_copyin(&new_time, time, sizeof(new_time));

    if(copied < 0) {
        return copied;
    }

    if(!cred_capable(thread->credentials, CAP_SYS_TIME)) {
        return -EPERM;
    }

    return wallclock_set(&new_time);
}

define_syscall(sys_settime, SYS_SETTIME, 2);

// Both clocks tick from the active clock source
// A null pointer only asks whether the clock exists, as in Linux
static intptr_t sys_gettimeres(uintptr_t clock, user_addr_t resolution)
{
    if(canonical_clock(clock) < 0) {
        return -EINVAL;
    }

    if(resolution.value == 0) {
        return 0;
    }

    timespec_t step;
    timespec_from_nsec(&step, clock_source_resolution_ns());

    ssize_t copied = user_copyout(resolution, &step, sizeof(step));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_gettimeres, SYS_GETTIMERES, 2);

// A stop does not end the sleep: the thread parks, and the time that passed while it was stopped counts
static ssize_t sleep_relative(uint64_t timeout, user_addr_t remaining_address)
{
    thread_t* thread = this_thread();
    uint64_t start = clock_source_now_ns();
    uint64_t left = timeout;
    ssize_t result = event_sleep(left);

    while(result == -ERESTART && job_resume_wait(thread)) {
        uint64_t elapsed = clock_source_now_ns() - start;

        if(elapsed >= timeout) {
            return 0;
        }

        left = timeout - elapsed;
        result = event_sleep(left);
    }

    if(result != -ERESTART) {
        return result;
    }

    if(remaining_address.value) {
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
    thread_t* thread = this_thread();

    for(;;) {
        uint64_t now = clock_source_now_ns();

        if(target <= now) {
            return 0;
        }

        ssize_t result = event_sleep(target - now);

        if(result == -ERESTART && job_resume_wait(thread)) {
            continue;
        }

        if(result == -ERESTART) {
            return -EINTR;
        }

        return result;
    }
}

static ssize_t sleep_until_realtime(const timespec_t* restrict target)
{
    thread_t* thread = this_thread();
    ssize_t result = wallclock_wait_until(target);

    while(result == -ERESTART && job_resume_wait(thread)) {
        result = wallclock_wait_until(target);
    }

    if(result == -ERESTART) {
        return -EINTR;
    }

    return result;
}

// A signal ends the sleep for good, as POSIX has it, so the call never restarts
static intptr_t sys_nanosleep(uintptr_t clock_id, uintptr_t flags, user_addr_t request, user_addr_t remaining)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    int clock = canonical_clock(clock_id);

    if(clock < 0) {
        return -EINVAL;
    }

    if(flags & ~(uintptr_t)(TIMER_ABSTIME)) {
        return -EINVAL;
    }

    timespec_t duration;
    ssize_t copied = user_copyin(&duration, request, sizeof(duration));

    if(copied < 0) {
        return copied;
    }

    if(!timespec_valid(&duration)) {
        return -EINVAL;
    }

    if(flags & TIMER_ABSTIME) {
        if(clock == CLOCK_REALTIME) {
            return sleep_until_realtime(&duration);
        }

        return sleep_until_monotonic(timespec_to_nsec(&duration));
    }

    uint64_t timeout = timespec_to_nsec(&duration);

    // Zero would mean to wait for as long as it takes
    if(timeout == 0) {
        sched_yield();
        return 0;
    }

    return sleep_relative(timeout, remaining);
}

define_syscall(sys_nanosleep, SYS_NANOSLEEP, 4);
