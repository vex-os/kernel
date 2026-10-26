#include <vex/errno.h>
#include <vex/syscall.h>
#include <vex/time.h>

#include "ktest.h"

#define SLEEP_NSEC                   500000000
#define NSEC_PER_SEC                 1000000000
#define NSEC_PER_ELAPSED(start, end) (((end).tv_sec - (start).tv_sec) * NSEC_PER_SEC + ((end).tv_nsec - (start).tv_nsec))

static long sleep_for(long seconds, long nanoseconds)
{
    struct __k_timespec request = {};

    request.tv_sec = seconds;
    request.tv_nsec = nanoseconds;

    return ktest_syscall2(__k_SYS_SLEEP, (long)(&request), 0);
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
    ktest_expect(ktest_syscall2(__k_SYS_SLEEP, 0, 0), -__k_EFAULT);

    return ktest_result();
}
