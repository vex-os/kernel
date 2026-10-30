#pragma once

#include <usr/vex/time.h>
#include <vex/clocksrc.h>
#include <vex/compiler.h>
#include <vex/types.h>

#define CLOCK_REALTIME           __k_CLOCK_REALTIME
#define CLOCK_MONOTONIC          __k_CLOCK_MONOTONIC
#define CLOCK_PROCESS_CPUTIME_ID __k_CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID  __k_CLOCK_THREAD_CPUTIME_ID
#define CLOCK_MONOTONIC_RAW      __k_CLOCK_MONOTONIC_RAW
#define CLOCK_REALTIME_COARSE    __k_CLOCK_REALTIME_COARSE
#define CLOCK_MONOTONIC_COARSE   __k_CLOCK_MONOTONIC_COARSE
#define CLOCK_BOOTTIME           __k_CLOCK_BOOTTIME

#define TIMER_ABSTIME __k_TIMER_ABSTIME

#define ITIMER_REAL    __k_ITIMER_REAL
#define ITIMER_VIRTUAL __k_ITIMER_VIRTUAL
#define ITIMER_PROF    __k_ITIMER_PROF

#define NSEC_PER_SEC  INT64_C(1000000000)
#define NSEC_PER_USEC INT64_C(1000)

typedef struct __k_timespec timespec_t;
typedef struct __k_timeval timeval_t;
typedef struct __k_itimerspec itimerspec_t;

static __always_inline void timespec_from_nsec(timespec_t* restrict out, uint64_t nsec)
{
    out->tv_sec = (int64_t)(nsec / NSEC_PER_SEC);
    out->tv_nsec = (int64_t)(nsec % NSEC_PER_SEC);
}

/// Cuts the nanoseconds down to whole microseconds
static __always_inline void timeval_from_nsec(timeval_t* restrict out, uint64_t nsec)
{
    out->tv_sec = (int64_t)(nsec / NSEC_PER_SEC);
    out->tv_usec = (int64_t)(nsec % NSEC_PER_SEC / NSEC_PER_USEC);
}

/// Whether the fields are in range, as POSIX requires for a request
static __always_inline bool timespec_valid(const timespec_t* restrict ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < NSEC_PER_SEC;
}

/// Saturates at UINT64_MAX. The timespec must be valid
static __always_inline uint64_t timespec_to_nsec(const timespec_t* restrict ts)
{
    uint64_t seconds = ts->tv_sec;

    if(seconds >= (UINT64_MAX - ts->tv_nsec) / NSEC_PER_SEC) {
        return UINT64_MAX;
    }

    return seconds * NSEC_PER_SEC + ts->tv_nsec;
}

/// A point on the clock source, saturating at
// UINT64_MAX. Never zero, which the waits take as no deadline
static __always_inline uint64_t deadline_after_nsec(uint64_t timeout_nsec)
{
    uint64_t now = clock_source_now_ns();

    if(now > UINT64_MAX - timeout_nsec) {
        return UINT64_MAX;
    }

    uint64_t deadline = now + timeout_nsec;

    if(deadline == 0) {
        return 1;
    }

    return deadline;
}
