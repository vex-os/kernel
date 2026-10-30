#include <vex/errno.h>
#include <vex/syscall.h>
#include <vex/time.h>

#include "ktest.h"

#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_SEC  1000000000L
#define BUSY_NSEC     (200 * NSEC_PER_MSEC)
#define SLEEP_NSEC    (100 * NSEC_PER_MSEC)
#define SLACK_NSEC    (5 * NSEC_PER_MSEC) // The cycle counter and the clock source can disagree a little

static long read_clock(long clock)
{
    struct __k_timespec time = {};
    long retval = ktest_syscall2(__k_SYS_GETTIME, clock, (long)(&time));
    ktest_expect(retval, 0);
    return time.tv_sec * NSEC_PER_SEC + time.tv_nsec;
}

static void spin_for(long nsec)
{
    long start = read_clock(__k_CLOCK_MONOTONIC);

    while(read_clock(__k_CLOCK_MONOTONIC) - start < nsec) {
        // empty
    }
}

static void test_resolution(long clock)
{
    struct __k_timespec resolution = {};

    ktest_expect(ktest_syscall2(__k_SYS_GETTIMERES, clock, (long)(&resolution)), 0);
    ktest_expect(resolution.tv_sec, 0);
    ktest_assert(resolution.tv_nsec >= 1);
    ktest_expect(ktest_syscall2(__k_SYS_GETTIMERES, clock, 0), 0);
}

// Each read goes through the kernel, so the clock of the thread
// never goes back, and a busy thread gets most of the wall time
static void test_busy(void)
{
    long wall_start = read_clock(__k_CLOCK_MONOTONIC);
    long thread_start = read_clock(__k_CLOCK_THREAD_CPUTIME_ID);
    long previous = thread_start;
    bool monotonic = true;

    while(read_clock(__k_CLOCK_MONOTONIC) - wall_start < BUSY_NSEC) {
        long now = read_clock(__k_CLOCK_THREAD_CPUTIME_ID);
        monotonic = monotonic && now >= previous;
        previous = now;
    }

    long wall = read_clock(__k_CLOCK_MONOTONIC) - wall_start;
    long used = read_clock(__k_CLOCK_THREAD_CPUTIME_ID) - thread_start;

    ktest_assert(monotonic);
    ktest_assert(used <= wall + SLACK_NSEC);
    ktest_assert(used >= wall / 4);
}

static void test_sleep(void)
{
    struct __k_timespec request = {
        .tv_sec = 0,
        .tv_nsec = SLEEP_NSEC,
    };

    long thread_start = read_clock(__k_CLOCK_THREAD_CPUTIME_ID);
    ktest_expect(ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0), 0);
    long used = read_clock(__k_CLOCK_THREAD_CPUTIME_ID) - thread_start;

    ktest_assert(used < SLEEP_NSEC / 4);
}

static void test_process(void)
{
    spin_for(10 * NSEC_PER_MSEC);

    long thread_time = read_clock(__k_CLOCK_THREAD_CPUTIME_ID);
    long process_time = read_clock(__k_CLOCK_PROCESS_CPUTIME_ID);

    ktest_assert(thread_time > 0);
    ktest_assert(process_time >= thread_time);
}

int main(void)
{
    test_resolution(__k_CLOCK_THREAD_CPUTIME_ID);
    test_resolution(__k_CLOCK_PROCESS_CPUTIME_ID);

    test_busy();
    test_sleep();
    test_process();

    // No wait goes by a CPU time clock
    struct __k_timespec request = {};
    ktest_expect(ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_THREAD_CPUTIME_ID, 0, (long)(&request), 0), -__k_EINVAL);

    return ktest_result();
}
