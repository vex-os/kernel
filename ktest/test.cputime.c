#include <vex/errno.h>
#include <vex/mman.h>
#include <vex/resource.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_SEC  1000000000L
#define BUSY_NSEC     (200 * NSEC_PER_MSEC)
#define SLEEP_NSEC    (100 * NSEC_PER_MSEC)
#define CHILD_NSEC    (50 * NSEC_PER_MSEC)
#define PAGE_SIZE     4096
#define TOUCH_PAGES   16
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

static long usec_of(const struct __k_timeval* time)
{
    return time->tv_sec * 1000000L + time->tv_usec;
}

static void get_usage(long who, struct __k_rusage* usage)
{
    ktest_expect(ktest_syscall2(__k_SYS_GETRUSAGE, who, (long)(usage)), 0);
}

// Reads the clock only every so often, so that nearly all of the time is in user mode
static void spin_in_user(long nsec)
{
    long start = read_clock(__k_CLOCK_MONOTONIC);

    do {
        for(volatile long i = 0; i < 100000; ++i) {
            // empty
        }
    } while(read_clock(__k_CLOCK_MONOTONIC) - start < nsec);
}

static void spin_in_kernel(long nsec)
{
    long start = read_clock(__k_CLOCK_MONOTONIC);

    while(read_clock(__k_CLOCK_MONOTONIC) - start < nsec) {
        for(int i = 0; i < 100; ++i) {
            ktest_syscall1(__k_SYS_GETPID, 0);
        }
    }
}

static void test_user_and_system(void)
{
    struct __k_rusage before = {};
    struct __k_rusage after = {};

    get_usage(__k_RUSAGE_THREAD, &before);
    spin_in_user(BUSY_NSEC / 2);
    get_usage(__k_RUSAGE_THREAD, &after);

    long user = usec_of(&after.ru_utime) - usec_of(&before.ru_utime);
    long system = usec_of(&after.ru_stime) - usec_of(&before.ru_stime);

    ktest_assert(user > system);

    get_usage(__k_RUSAGE_THREAD, &before);
    spin_in_kernel(BUSY_NSEC / 2);
    get_usage(__k_RUSAGE_THREAD, &after);

    system = usec_of(&after.ru_stime) - usec_of(&before.ru_stime);

    ktest_assert(system > 0);
}

static void test_usage(void)
{
    struct __k_rusage thread = {};
    struct __k_rusage self = {};

    get_usage(__k_RUSAGE_THREAD, &thread);
    get_usage(__k_RUSAGE_SELF, &self);

    ktest_assert(usec_of(&self.ru_utime) >= usec_of(&thread.ru_utime));
    ktest_assert(usec_of(&self.ru_stime) >= usec_of(&thread.ru_stime));
    ktest_expect(self.ru_maxrss, 0);
    ktest_expect(self.ru_inblock, 0);
    ktest_expect(ktest_syscall2(__k_SYS_GETRUSAGE, 7, (long)(&self)), -__k_EINVAL);
    ktest_expect(ktest_syscall2(__k_SYS_GETRUSAGE, __k_RUSAGE_SELF, 1), -__k_EFAULT);
}

static long total_usec(const struct __k_rusage* usage)
{
    return usec_of(&usage->ru_utime) + usec_of(&usage->ru_stime);
}

static long fork_busy_child(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        spin_in_user(CHILD_NSEC);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_assert(child > 0);
    return child;
}

// The parent gets the times of a child once it collects it, from either call
static void test_children(void)
{
    struct __k_rusage children_before = {};
    get_usage(__k_RUSAGE_CHILDREN, &children_before);

    long child = fork_busy_child();
    struct __k_rusage usage = {};
    int wstatus = -1;

    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, (long)(&usage)), child);
    ktest_expect(wstatus, 0);
    ktest_assert(total_usec(&usage) >= CHILD_NSEC / 1000 / 4);

    struct __k_rusage children_after = {};
    get_usage(__k_RUSAGE_CHILDREN, &children_after);

    long collected = total_usec(&children_after) - total_usec(&children_before);
    // Each field is cut to microseconds on its own, so the sums can differ by a few
    long difference = collected - total_usec(&usage);
    ktest_assert(difference >= -4 && difference <= 4);

    child = fork_busy_child();
    usage = (struct __k_rusage) {};

    struct __k_siginfo info = {};
    ktest_expect(ktest_syscall5(__k_SYS_WAITID, __k_P_PID, child, (long)(&info), __k_WEXITED, (long)(&usage)), 0);
    ktest_expect(info.si_pid, child);
    ktest_assert(total_usec(&usage) >= CHILD_NSEC / 1000 / 4);
}

// A stop reports the time that the child has used so far
static void test_stopped_child(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        spin_in_user(10 * NSEC_PER_SEC);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_assert(child > 0);

    struct __k_timespec request = {
        .tv_sec = 0,
        .tv_nsec = CHILD_NSEC,
    };

    ktest_expect(ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0), 0);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGSTOP), 0);

    struct __k_rusage usage = {};
    int wstatus = -1;

    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), __k_WUNTRACED, (long)(&usage)), child);
    ktest_expect(__k_WIFSTOPPED(wstatus), 1);
    ktest_assert(total_usec(&usage) > 0);

    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL), 0);
    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0), child);
}

static volatile int handled;

static void on_signal(__k_int32_t sig)
{
    handled += 1;
}

// Each page of fresh anonymous memory faults once on its first write
static void touch_fresh_pages(void)
{
    long length = TOUCH_PAGES * PAGE_SIZE;
    long address = ktest_syscall6(__k_SYS_MMAP, 0, length, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_PRIVATE | __k_MAP_ANONYMOUS, -1, 0);

    ktest_assert(address > 0);

    for(long offset = 0; offset < length; offset += PAGE_SIZE) {
        *(volatile char*)(address + offset) = 1;
    }

    ktest_expect(ktest_syscall2(__k_SYS_MUNMAP, address, length), 0);
}

static void test_faults(void)
{
    struct __k_rusage before = {};
    struct __k_rusage after = {};

    get_usage(__k_RUSAGE_THREAD, &before);
    touch_fresh_pages();
    get_usage(__k_RUSAGE_THREAD, &after);

    ktest_assert(after.ru_minflt - before.ru_minflt >= TOUCH_PAGES);
    ktest_expect(after.ru_majflt, 0);
}

static void test_switches(void)
{
    struct __k_rusage before = {};
    struct __k_rusage after = {};
    struct __k_timespec request = {
        .tv_sec = 0,
        .tv_nsec = NSEC_PER_MSEC,
    };

    get_usage(__k_RUSAGE_THREAD, &before);

    for(int i = 0; i < 3; ++i) {
        ktest_expect(ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0), 0);
    }

    get_usage(__k_RUSAGE_THREAD, &after);

    ktest_assert(after.ru_nvcsw - before.ru_nvcsw >= 3);
}

static void test_signals(void)
{
    struct __k_sigaction action = {};
    action.sa_handler = &on_signal;
    action.sa_flags = __k_SA_RESTORER;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_expect(ktest_syscall3(__k_SYS_SIGACTION, __k_SIGUSR1, (long)(&action), 0), 0);

    struct __k_rusage before = {};
    struct __k_rusage after = {};

    get_usage(__k_RUSAGE_SELF, &before);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, ktest_syscall1(__k_SYS_GETPID, 0), __k_SIGUSR1), 0);
    get_usage(__k_RUSAGE_SELF, &after);

    ktest_expect(handled, 1);
    ktest_expect(after.ru_nsignals - before.ru_nsignals, 1);
}

// The counters of a child reach the parent with its times
static void test_child_counters(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        touch_fresh_pages();
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_assert(child > 0);

    struct __k_rusage usage = {};
    int wstatus = -1;

    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, (long)(&usage)), child);
    ktest_assert(usage.ru_minflt >= TOUCH_PAGES);
}

int main(void)
{
    test_resolution(__k_CLOCK_THREAD_CPUTIME_ID);
    test_resolution(__k_CLOCK_PROCESS_CPUTIME_ID);

    test_busy();
    test_sleep();
    test_process();
    test_user_and_system();
    test_usage();
    test_children();
    test_stopped_child();
    test_faults();
    test_switches();
    test_signals();
    test_child_counters();

    // No wait goes by a CPU time clock
    struct __k_timespec request = {};
    ktest_expect(ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_THREAD_CPUTIME_ID, 0, (long)(&request), 0), -__k_EINVAL);

    return ktest_result();
}
