#include <vex/errno.h>
#include <vex/syscall.h>
#include <vex/time.h>

#include "ktest.h"

#define SLEEP_NSEC                   500000000
#define NSEC_PER_SEC                 1000000000
#define NSEC_PER_ELAPSED(start, end) (((end).tv_sec - (start).tv_sec) * NSEC_PER_SEC + ((end).tv_nsec - (start).tv_nsec))

static long sleep_clock(long clock, long flags, long seconds, long nanoseconds)
{
    struct __k_timespec request = {};

    request.tv_sec = seconds;
    request.tv_nsec = nanoseconds;

    return ktest_syscall4(__k_SYS_NANOSLEEP, clock, flags, (long)(&request), 0);
}

static long sleep_for(long seconds, long nanoseconds)
{
    return sleep_clock(__k_CLOCK_MONOTONIC, 0, seconds, nanoseconds);
}

static long sleep_until(long clock, const struct __k_timespec* point)
{
    return sleep_clock(clock, __k_TIMER_ABSTIME, point->tv_sec, point->tv_nsec);
}

static void check_absolute(long clock)
{
    struct __k_timespec point = {};
    struct __k_timespec end = {};

    ktest_expect(ktest_syscall2(__k_SYS_GETTIME, clock, (long)(&point)), 0);

    // A point in the past returns at once
    ktest_expect(sleep_until(clock, &point), 0);

    point.tv_nsec += SLEEP_NSEC;

    if(point.tv_nsec >= NSEC_PER_SEC) {
        point.tv_nsec -= NSEC_PER_SEC;
        point.tv_sec += 1;
    }

    ktest_expect(sleep_until(clock, &point), 0);
    ktest_expect(ktest_syscall2(__k_SYS_GETTIME, clock, (long)(&end)), 0);
    ktest_expect(point.tv_sec < end.tv_sec || (point.tv_sec == end.tv_sec && point.tv_nsec <= end.tv_nsec), 1);
}

int main(void)
{
    struct __k_timespec start = {};
    struct __k_timespec end = {};

    ktest_expect(ktest_syscall2(__k_SYS_GETTIME, __k_CLOCK_MONOTONIC, (long)(&start)), 0);
    ktest_expect(sleep_for(0, SLEEP_NSEC), 0);
    ktest_expect(ktest_syscall2(__k_SYS_GETTIME, __k_CLOCK_MONOTONIC, (long)(&end)), 0);

    // The sleep may last longer than asked, never shorter
    ktest_expect(NSEC_PER_ELAPSED(start, end) >= SLEEP_NSEC, 1);

    ktest_expect(sleep_for(0, 0), 0);
    ktest_expect(sleep_for(-1, 0), -__k_EINVAL);
    ktest_expect(sleep_for(0, NSEC_PER_SEC), -__k_EINVAL);
    ktest_expect(ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, 0, 0), -__k_EFAULT);
    ktest_expect(sleep_clock(2, 0, 0, 0), -__k_EINVAL);
    ktest_expect(sleep_clock(__k_CLOCK_MONOTONIC, 2, 0, 0), -__k_EINVAL);
    ktest_expect(sleep_clock(__k_CLOCK_REALTIME, __k_TIMER_ABSTIME, 0, NSEC_PER_SEC), -__k_EINVAL);

    check_absolute(__k_CLOCK_MONOTONIC);
    check_absolute(__k_CLOCK_REALTIME);

    return ktest_result();
}
