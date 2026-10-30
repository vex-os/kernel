#include <vex/errno.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define MAX_TEST_HARTS 8
#define SETTLE_COUNT   2000
#define SAMPLE_COUNT   200
#define BURN_COUNT     20000
#define NAP_NSEC       1000000L
#define NAP_EVERY      10
#define NOT_SET        (-1L)

static volatile long worker_tid = NOT_SET;
static volatile unsigned long worker_inherited;
static volatile unsigned long worker_wanted;
static volatile long worker_stays;
static volatile int worker_go;
static volatile int worker_done;
static volatile int worker_started;

static long set_mask(long pid, long tid, unsigned long mask)
{
    return ktest_syscall4(__k_SYS_SETAFFINITY, pid, tid, sizeof(mask), (long)(&mask));
}

static unsigned long get_mask(long pid, long tid)
{
    unsigned long mask = 0;

    ktest_expect(ktest_syscall4(__k_SYS_GETAFFINITY, pid, tid, sizeof(mask), (long)(&mask)), sizeof(mask));

    return mask;
}

static long current_hart(void)
{
    return ktest_syscall1(__k_SYS_GETCPU, 0);
}

static void burn(void)
{
    for(int i = 0; i < BURN_COUNT; ++i) {
        __asm__ volatile("");
    }
}

static void nap(void)
{
    struct __k_timespec request = {};
    request.tv_nsec = NAP_NSEC;

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0);
}

// A thread that no hart picks up would hang the test, so every wait for another thread ends
static bool wait_for(volatile int* flag)
{
    for(int i = 0; i < SETTLE_COUNT && !*flag; ++i) {
        burn();
        ktest_syscall1(__k_SYS_YIELD, 0);
    }

    return *flag;
}

// The thread leaves a hart that its mask no longer holds at its next tick
static bool settle_on(long hart)
{
    for(int i = 0; i < SETTLE_COUNT; ++i) {
        if(current_hart() == hart) {
            return true;
        }

        burn();
        ktest_syscall1(__k_SYS_YIELD, 0);
    }

    return false;
}

// Runs, yields and sleeps, and counts the samples that found the thread on another hart
static long samples_elsewhere(long hart)
{
    long elsewhere = 0;

    for(int i = 0; i < SAMPLE_COUNT; ++i) {
        burn();

        if(i % NAP_EVERY == 0) {
            nap();
        }
        else {
            ktest_syscall1(__k_SYS_YIELD, 0);
        }

        if(current_hart() != hart) {
            elsewhere += 1;
        }
    }

    return elsewhere;
}

[[noreturn]] static void worker(long arg)
{
    worker_tid = ktest_syscall1(__k_SYS_GETTID, 0);
    worker_inherited = get_mask(0, 0);
    worker_started = 1;

    while(!worker_go) {
        ktest_syscall1(__k_SYS_YIELD, 0);
    }

    long wanted = __builtin_ctzl(worker_wanted);

    if(settle_on(wanted)) {
        worker_stays = samples_elsewhere(wanted);
    }

    worker_done = 1;
    ktest_syscall1(__k_SYS_THREAD_EXIT, 0);
    __builtin_unreachable();
}

static void test_pinning(unsigned long online)
{
    for(long hart = 0; hart < MAX_TEST_HARTS; ++hart) {
        unsigned long bit = 1UL << hart;

        if(0 == (online & bit)) {
            continue;
        }

        ktest_expect(set_mask(0, 0, bit), 0);
        ktest_expect(get_mask(0, 0), bit);
        ktest_expect(settle_on(hart), 1);
        ktest_expect(samples_elsewhere(hart), 0);
    }

    // Every hart again
    ktest_expect(set_mask(0, 0, online), 0);
    ktest_expect(get_mask(0, 0), online);
}

static void test_fork(long hart)
{
    unsigned long bit = 1UL << hart;

    ktest_expect(set_mask(0, 0, bit), 0);
    ktest_expect(settle_on(hart), 1);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        // The child asks about the mask it got, and stays where it says
        unsigned long inherited = get_mask(0, 0);
        long elsewhere = NOT_SET;

        if(settle_on(hart)) {
            elsewhere = samples_elsewhere(hart);
        }

        ktest_syscall1(__k_SYS_EXIT, inherited == bit && elsewhere == 0 ? 0 : 1);
    }

    int wstatus = -1;

    ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0);
    ktest_expect(wstatus, 0);
}

// A thread starts with the mask of its creator, and after that the two are not tied to each other
static void test_thread(long first, long second)
{
    unsigned long first_bit = 1UL << first;
    unsigned long second_bit = 1UL << second;
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);

    ktest_expect(set_mask(0, 0, first_bit), 0);
    ktest_expect(settle_on(first), 1);

    worker_tid = NOT_SET;
    worker_stays = NOT_SET;
    worker_wanted = second_bit;
    worker_go = 0;

    worker_done = 0;
    worker_started = 0;

    ktest_spawn_joinable(&worker, 0);

    ktest_expect(wait_for(&worker_started), 1);

    ktest_expect(worker_inherited, first_bit);

    // The mask of the worker is its own: setting it leaves the creator where it is
    ktest_expect(get_mask(pid, worker_tid), first_bit);
    ktest_expect(set_mask(pid, worker_tid, second_bit), 0);
    ktest_expect(get_mask(pid, worker_tid), second_bit);
    ktest_expect(get_mask(0, 0), first_bit);

    worker_go = 1;

    ktest_expect(samples_elsewhere(first), 0);
    ktest_expect(wait_for(&worker_done), 1);

    ktest_expect(worker_stays, 0);

    ktest_expect(set_mask(0, 0, first_bit | second_bit), 0);
}

static void test_errors(unsigned long online)
{
    unsigned long mask = 0;
    unsigned long none = 0;
    unsigned long missing = 1UL << 63;
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);

    ktest_expect(set_mask(0, 0, none), -__k_EINVAL);
    ktest_expect(set_mask(0, 0, missing), -__k_EINVAL);
    ktest_expect(set_mask(-1, 0, online), -__k_EINVAL);
    ktest_expect(set_mask(0, -1, online), -__k_EINVAL);
    ktest_expect(set_mask(0, 1L << 28, online), -__k_ESRCH);
    ktest_expect(set_mask(1L << 28, 0, online), -__k_ESRCH);
    ktest_expect(ktest_syscall4(__k_SYS_GETAFFINITY, pid, 1L << 28, sizeof(mask), (long)(&mask)), -__k_ESRCH);

    // Bits for harts that do not exist do not count, and the answer shows only the ones that do
    ktest_expect(set_mask(0, 0, online | missing), 0);
    ktest_expect(get_mask(0, 0), online);
}

int main(void)
{
    unsigned long online = get_mask(0, 0);
    long first = -1;
    long second = -1;

    for(long hart = 0; hart < MAX_TEST_HARTS; ++hart) {
        if(online & (1UL << hart)) {
            if(first < 0) {
                first = hart;
            }
            else if(second < 0) {
                second = hart;
            }
        }
    }

    ktest_expect(first >= 0, 1);
    ktest_expect(current_hart() >= 0, 1);

    test_errors(online);

    // One hart gives nothing to move between
    if(second >= 0) {
        test_pinning(online);
        test_fork(second);
        test_thread(first, second);
    }

    return ktest_result();
}
