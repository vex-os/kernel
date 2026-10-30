#include <vex/capability.h>
#include <vex/clocksrc.h>
#include <vex/cred.h>
#include <vex/cycles.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/itimer.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/uaccess.h>
#include <vex/usage.h>
#include <vex/wallclock.h>

#include "syscall_internal.h"

// The kernel steps the realtime clock and never slews, so the raw and coarse clocks read as the fine ones
// TODO: CLOCK_BOOTTIME is not an alias once suspend exists, as it counts the time spent suspended
// The CPU time clocks are not here: they only read, and no wait goes by them
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

static bool is_cputime_clock(uintptr_t clock)
{
    return clock == CLOCK_THREAD_CPUTIME_ID || clock == CLOCK_PROCESS_CPUTIME_ID;
}

static int read_cputime_clock(uintptr_t clock, timespec_t* restrict now)
{
    thread_t* thread = this_thread();
    usage_t time = {};

    if(clock == CLOCK_THREAD_CPUTIME_ID) {
        usage_of_thread(thread, &time);
    }
    else if(thread->process) {
        usage_of_process(thread->process, &time);
    }
    else {
        return -EINVAL;
    }

    timespec_from_nsec(now, time.user_ns + time.system_ns);
    return 0;
}

static intptr_t sys_gettime(uintptr_t clock, user_addr_t time)
{
    timespec_t now;

    if(is_cputime_clock(clock)) {
        int retval = read_cputime_clock(clock, &now);

        if(retval < 0) {
            return retval;
        }

        ssize_t copied = user_copyout(time, &now, sizeof(now));

        if(copied < 0) {
            return copied;
        }

        return 0;
    }

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

// The wall clocks tick from the active clock source, and
// the CPU time clocks from the clock of the hart. A null
// pointer only asks whether the clock exists, as in Linux
static intptr_t sys_gettimeres(uintptr_t clock, user_addr_t resolution)
{
    bool cputime = is_cputime_clock(clock);

    if(!cputime && canonical_clock(clock) < 0) {
        return -EINVAL;
    }

    if(resolution.value == 0) {
        return 0;
    }

    uint64_t step_ns = clock_source_resolution_ns();

    if(cputime) {
        step_ns = local_clock_resolution_ns();
    }

    timespec_t step;
    timespec_from_nsec(&step, step_ns);

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

static int read_itimer_state(user_addr_t source, itimer_state_t* restrict state)
{
    itimerspec_t request;
    ssize_t copied = user_copyin(&request, source, sizeof(request));

    if(copied < 0) {
        return copied;
    }

    if(!timespec_valid(&request.it_value) || !timespec_valid(&request.it_interval)) {
        return -EINVAL;
    }

    state->value = timespec_to_nsec(&request.it_value);
    state->interval = timespec_to_nsec(&request.it_interval);

    return 0;
}

// Reads the timer too: a null new setting changes nothing, and getitimer is that call
static intptr_t sys_setitimer(int which, user_addr_t value, user_addr_t old)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(which != ITIMER_REAL) {
        return -EINVAL;
    }

    itimer_state_t next;
    itimer_state_t previous;
    const itimer_state_t* request = nullptr;

    if(value.value) {
        int retval = read_itimer_state(value, &next);

        if(retval < 0) {
            return retval;
        }

        request = &next;
    }

    itimer_set(process, request, &previous);

    if(old.value) {
        itimerspec_t reply;

        timespec_from_nsec(&reply.it_value, previous.value);
        timespec_from_nsec(&reply.it_interval, previous.interval);

        ssize_t copied = user_copyout(old, &reply, sizeof(reply));

        if(copied < 0) {
            return copied;
        }
    }

    return 0;
}

define_syscall(sys_setitimer, SYS_SETITIMER, 3);
