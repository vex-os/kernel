#include <vex/errno.h>
#include <vex/resource.h>
#include <vex/sched.h>
#include <vex/syscall.h>
#include <vex/time.h>

#include "ktest.h"

#define UNPRIVILEGED_ID 1000
#define NO_SUCH_ID      999999
#define PARENT_WAIT_NS  100000000
#define CHILD_SLEEP_NS  400000000

static long get_nice_encoded(long which, long who)
{
    return ktest_syscall2(__k_SYS_GETPRIORITY, which, who);
}

static long set_nice(long which, long who, long nice)
{
    return ktest_syscall3(__k_SYS_SETPRIORITY, which, who, nice);
}

static long own_nice_encoded(void)
{
    return get_nice_encoded(__k_PRIO_PROCESS, 0);
}

static long interval_nsec(long pid)
{
    struct __k_timespec interval = {};

    ktest_expect(ktest_syscall2(__k_SYS_GETSCHEDINTERVAL, pid, (long)(&interval)), 0);

    return interval.tv_sec * 1000000000 + interval.tv_nsec;
}

static void sleep_nsec(long nanoseconds)
{
    struct __k_timespec request = {};

    request.tv_nsec = nanoseconds;
    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0);
}

static void wait_ok(long child)
{
    int wstatus = -1;

    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0), child);
    ktest_expect(wstatus, 0);
}

static void test_nice(void)
{
    ktest_expect(own_nice_encoded(), __k_NICE_BIAS);

    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 5), 0);
    ktest_expect(own_nice_encoded(), __k_NICE_BIAS - 5);

    // A value out of range counts as the nearest end
    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 100), 0);
    ktest_expect(own_nice_encoded(), __k_NICE_BIAS - __k_NICE_MAX);
    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, -100), 0);
    ktest_expect(own_nice_encoded(), __k_NICE_BIAS - __k_NICE_MIN);

    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 0), 0);

    ktest_expect(set_nice(7, 0, 0), -__k_EINVAL);
    ktest_expect(get_nice_encoded(7, 0), -__k_EINVAL);
    ktest_expect(get_nice_encoded(__k_PRIO_PROCESS, NO_SUCH_ID), -__k_ESRCH);
    ktest_expect(get_nice_encoded(__k_PRIO_PROCESS, -1), -__k_ESRCH);
    ktest_expect(get_nice_encoded(__k_PRIO_PGRP, NO_SUCH_ID), -__k_ESRCH);
    ktest_expect(get_nice_encoded(__k_PRIO_USER, NO_SUCH_ID), -__k_ESRCH);
    ktest_expect(set_nice(__k_PRIO_PROCESS, NO_SUCH_ID, 0), -__k_ESRCH);
}

static void test_timeslice(void)
{
    long normal = interval_nsec(0);

    ktest_expect(normal > 0, 1);

    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 10), 0);
    ktest_expect(interval_nsec(0) < normal, 1);

    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, -10), 0);
    ktest_expect(interval_nsec(0) > normal, 1);

    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 0), 0);
    ktest_expect(interval_nsec(0), normal);

    ktest_expect(ktest_syscall2(__k_SYS_GETSCHEDINTERVAL, NO_SUCH_ID, 0), -__k_ESRCH);
}

static void test_children(void)
{
    // A child starts with the nice value of its parent
    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 3), 0);

    long inheriting = ktest_syscall1(__k_SYS_FORK, 0);

    if(inheriting == 0) {
        ktest_expect(own_nice_encoded(), __k_NICE_BIAS - 3);
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    wait_ok(inheriting);
    ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 0), 0);

    // Another process in a group of its own is reached through its user, and a child in ours through the group
    long other_group = ktest_syscall1(__k_SYS_FORK, 0);

    if(other_group == 0) {
        ktest_syscall1(__k_SYS_SETSID, 0);
        sleep_nsec(CHILD_SLEEP_NS);
        ktest_expect(own_nice_encoded(), __k_NICE_BIAS - 4);
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    long same_group = ktest_syscall1(__k_SYS_FORK, 0);

    if(same_group == 0) {
        sleep_nsec(CHILD_SLEEP_NS);
        ktest_expect(own_nice_encoded(), __k_NICE_BIAS - 6);
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    sleep_nsec(PARENT_WAIT_NS);

    ktest_expect(set_nice(__k_PRIO_USER, 0, 4), 0);
    ktest_expect(get_nice_encoded(__k_PRIO_USER, 0), __k_NICE_BIAS - 4);
    ktest_expect(set_nice(__k_PRIO_PGRP, 0, 6), 0);
    ktest_expect(get_nice_encoded(__k_PRIO_PGRP, 0), __k_NICE_BIAS - 6);

    // The other child is in no group of ours, so it still has what its user set
    ktest_expect(get_nice_encoded(__k_PRIO_PROCESS, other_group), __k_NICE_BIAS - 4);
    ktest_expect(get_nice_encoded(__k_PRIO_PROCESS, same_group), __k_NICE_BIAS - 6);

    wait_ok(other_group);
    wait_ok(same_group);

    // The calls above reached the test runner and init too
    ktest_expect(set_nice(__k_PRIO_USER, 0, 0), 0);
}

static void test_unprivileged(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        // The limit is set before the capability goes, and lets the process lower its nice value down to 15
        struct __k_rlimit limit = {};
        limit.rlim_cur = 5;
        limit.rlim_max = 5;
        ktest_expect(ktest_syscall2(__k_SYS_SETRLIMIT, __k_RLIMIT_NICE, (long)(&limit)), 0);

        ktest_expect(ktest_syscall3(__k_SYS_SETRESUID, UNPRIVILEGED_ID, UNPRIVILEGED_ID, UNPRIVILEGED_ID), 0);

        // Only a process with CAP_SYS_NICE lowers the nice value, or touches a process of someone else
        ktest_expect(set_nice(__k_PRIO_PROCESS, 0, -1), -__k_EACCES);
        ktest_expect(own_nice_encoded(), __k_NICE_BIAS);
        ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 5), 0);
        ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 2), -__k_EACCES);
        ktest_expect(own_nice_encoded(), __k_NICE_BIAS - 5);
        ktest_expect(set_nice(__k_PRIO_PROCESS, ktest_syscall1(__k_SYS_GETPPID, 0), 5), -__k_EPERM);

        // RLIMIT_NICE of 5 lets it go down to 20 - 5, and no further
        ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 19), 0);
        ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 15), 0);
        ktest_expect(set_nice(__k_PRIO_PROCESS, 0, 14), -__k_EACCES);
        ktest_expect(own_nice_encoded(), __k_NICE_BIAS - 15);

        // The same goes for a policy and a mask
        struct __k_sched_param param = {};
        unsigned long mask = 1;

        ktest_expect(ktest_syscall3(__k_SYS_SETSCHEDULER, 0, __k_SCHED_OTHER, (long)(&param)), 0);
        ktest_expect(ktest_syscall3(__k_SYS_SETSCHEDULER, ktest_syscall1(__k_SYS_GETPPID, 0), __k_SCHED_OTHER, (long)(&param)), -__k_EPERM);
        ktest_expect(ktest_syscall3(__k_SYS_SETAFFINITY, ktest_syscall1(__k_SYS_GETPPID, 0), sizeof(mask), (long)(&mask)), -__k_EPERM);

        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    wait_ok(child);
}

static void test_policy(void)
{
    struct __k_sched_param param = {};
    struct __k_sched_range range = { -1, -1 };

    ktest_expect(ktest_syscall1(__k_SYS_GETSCHEDULER, 0), __k_SCHED_OTHER);
    ktest_expect(ktest_syscall1(__k_SYS_GETSCHEDULER, -1), -__k_EINVAL);
    ktest_expect(ktest_syscall1(__k_SYS_GETSCHEDULER, NO_SUCH_ID), -__k_ESRCH);

    ktest_expect(ktest_syscall3(__k_SYS_SETSCHEDULER, 0, __k_SCHED_OTHER, (long)(&param)), 0);
    ktest_expect(ktest_syscall3(__k_SYS_SETSCHEDULER, 0, __k_SCHED_FIFO, (long)(&param)), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_SETSCHEDULER, 0, __k_SCHED_OTHER, 0), -__k_EFAULT);
    ktest_expect(ktest_syscall3(__k_SYS_SETSCHEDULER, NO_SUCH_ID, __k_SCHED_OTHER, (long)(&param)), -__k_ESRCH);

    param.sched_priority = 1;
    ktest_expect(ktest_syscall3(__k_SYS_SETSCHEDULER, 0, __k_SCHED_OTHER, (long)(&param)), -__k_EINVAL);
    ktest_expect(ktest_syscall2(__k_SYS_SETSCHEDPARAM, 0, (long)(&param)), -__k_EINVAL);

    param.sched_priority = 0;
    ktest_expect(ktest_syscall2(__k_SYS_SETSCHEDPARAM, 0, (long)(&param)), 0);

    param.sched_priority = 5;
    ktest_expect(ktest_syscall2(__k_SYS_GETSCHEDPARAM, 0, (long)(&param)), 0);
    ktest_expect(param.sched_priority, 0);

    ktest_expect(ktest_syscall2(__k_SYS_GETSCHEDRANGE, __k_SCHED_OTHER, (long)(&range)), 0);
    ktest_expect(range.min == 0 && range.max == 0, 1);
    ktest_expect(ktest_syscall2(__k_SYS_GETSCHEDRANGE, __k_SCHED_FIFO, (long)(&range)), -__k_EINVAL);
}

static void test_affinity(void)
{
    unsigned long mask = 0;
    unsigned long none = 0;
    unsigned long missing = 1UL << 63;

    // The call gives back the size of the mask, which is whole words, and the first hart is always there
    long size = ktest_syscall3(__k_SYS_GETAFFINITY, 0, sizeof(mask), (long)(&mask));

    ktest_expect(size > 0 && size % sizeof(mask) == 0, 1);
    ktest_expect(mask & 1, 1);

    ktest_expect(ktest_syscall3(__k_SYS_GETAFFINITY, 0, sizeof(mask) - 1, (long)(&mask)), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_GETAFFINITY, 0, 0, (long)(&mask)), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_GETAFFINITY, NO_SUCH_ID, sizeof(mask), (long)(&mask)), -__k_ESRCH);

    ktest_expect(ktest_syscall3(__k_SYS_SETAFFINITY, 0, sizeof(mask), (long)(&mask)), 0);
    ktest_expect(ktest_syscall3(__k_SYS_SETAFFINITY, 0, sizeof(none), (long)(&none)), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_SETAFFINITY, 0, sizeof(missing), (long)(&missing)), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_SETAFFINITY, 0, sizeof(mask), 0), -__k_EFAULT);
    ktest_expect(ktest_syscall3(__k_SYS_SETAFFINITY, NO_SUCH_ID, sizeof(mask), (long)(&mask)), -__k_ESRCH);
}

int main(void)
{
    test_nice();
    test_timeslice();
    test_children();
    test_unprivileged();
    test_policy();
    test_affinity();

    return ktest_result();
}
