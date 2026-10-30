#include <vex/errno.h>
#include <vex/syscall.h>
#include <vex/time.h>

#include "ktest.h"

#define NSEC_PER_SEC 1000000000

static long get_resolution(long clock, struct __k_timespec* resolution)
{
    return ktest_syscall2(__k_SYS_GETTIMERES, clock, (long)(resolution));
}

static long get_time(long clock, struct __k_timespec* time)
{
    return ktest_syscall2(__k_SYS_GETTIME, clock, (long)(time));
}

int main(void)
{
    static const long aliases[] = {
        __k_CLOCK_REALTIME,
        __k_CLOCK_MONOTONIC,
        __k_CLOCK_MONOTONIC_RAW,
        __k_CLOCK_REALTIME_COARSE,
        __k_CLOCK_MONOTONIC_COARSE,
    };

    for(unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        struct __k_timespec resolution = {};
        struct __k_timespec time = {};

        ktest_expect(get_resolution(aliases[i], &resolution), 0);
        ktest_expect(resolution.tv_sec == 0 && resolution.tv_nsec >= 1 && resolution.tv_nsec < NSEC_PER_SEC, 1);
        ktest_expect(get_time(aliases[i], &time), 0);

        // A null pointer only asks whether the clock exists
        ktest_expect(ktest_syscall2(__k_SYS_GETTIMERES, aliases[i], 0), 0);
    }

    struct __k_timespec resolution = {};

    ktest_expect(get_resolution(__k_CLOCK_BOOTTIME, &resolution), -__k_EINVAL);
    ktest_expect(get_resolution(99, &resolution), -__k_EINVAL);
    ktest_expect(get_time(__k_CLOCK_BOOTTIME, &resolution), -__k_EINVAL);
    ktest_expect(get_resolution(__k_CLOCK_MONOTONIC, (struct __k_timespec*)(1)), -__k_EFAULT);

    return ktest_result();
}
