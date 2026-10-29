#include <vex/errno.h>
#include <vex/mman.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define SETTLE_NSEC 80000000L
#define CHILD_SLEEP 300000000L
#define SHARED_RW   (__k_PROT_READ | __k_PROT_WRITE)
#define SHARED_ANON (__k_MAP_SHARED | __k_MAP_ANONYMOUS)
#define BIT(sig)    (1UL << ((sig) - 1))
#define FAST_ROUNDS 4

// Counters in memory that the parent and the child share; a counter that stops moving belongs to a stopped thread
struct counters {
    volatile unsigned long main_thread;
    volatile unsigned long other_thread;
    volatile unsigned long handled;
    volatile unsigned long hangups;
    volatile unsigned long continues;
    volatile unsigned long after_stop;
};

static struct counters* counters;

static void sleep_for(long nanoseconds)
{
    struct __k_timespec request;
    request.tv_sec = nanoseconds / 1000000000L;
    request.tv_nsec = nanoseconds % 1000000000L;

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)&request, 0);
}

static long wait_for(long child, long options, int* restrict status)
{
    return ktest_syscall3(__k_SYS_WAITPID, child, (long)status, options);
}

static void spin_other(long arg)
{
    for(;;) {
        counters->other_thread += 1;
    }
}

[[noreturn]] static void spin_main(bool with_thread)
{
    if(with_thread) {
        ktest_spawn(&spin_other, 0);
    }

    for(;;) {
        counters->main_thread += 1;
    }
}

static void on_signal(__k_int32_t sig)
{
    counters->handled += 1;
}

static void on_hangup(__k_int32_t sig)
{
    counters->hangups += 1;
}

static void on_continue(__k_int32_t sig)
{
    counters->continues += 1;
}

static void install(int sig, void (*handler)(__k_int32_t), unsigned long flags)
{
    struct __k_sigaction action = {};
    action.sa_handler = handler;
    action.sa_flags = __k_SA_RESTORER | flags;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_syscall3(__k_SYS_SIGACTION, sig, (long)&action, 0);
}

static void block(int sig)
{
    __k_sigset_t mask = BIT(sig);

    ktest_syscall3(__k_SYS_SIGPROCMASK, __k_SIG_BLOCK, (long)&mask, 0);
}

static __k_sigset_t pending(void)
{
    __k_sigset_t set = 0;

    ktest_syscall1(__k_SYS_SIGPENDING, (long)&set);

    return set;
}

// The stop is reported before every thread has parked, so the counters get a moment to settle first
static bool counters_frozen(void)
{
    sleep_for(SETTLE_NSEC / 4);

    unsigned long main_before = counters->main_thread;
    unsigned long other_before = counters->other_thread;

    sleep_for(SETTLE_NSEC);

    return main_before == counters->main_thread && other_before == counters->other_thread;
}

static bool counters_moving(void)
{
    unsigned long main_before = counters->main_thread;

    sleep_for(SETTLE_NSEC);

    return main_before != counters->main_thread;
}

static long start_spinner(bool with_thread)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        spin_main(with_thread);
    }

    // The child is running once its counter moves
    while(counters->main_thread == 0) {
        ktest_syscall1(__k_SYS_YIELD, 0);
    }

    return child;
}

static void test_stop_and_continue(void)
{
    int status = 0;
    long child = start_spinner(true);

    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGSTOP), 0);
    ktest_expect(wait_for(child, __k_WUNTRACED, &status), child);
    ktest_expect(__k_WIFSTOPPED(status), 1);
    ktest_expect(__k_WSTOPSIG(status), __k_SIGSTOP);
    ktest_expect(__k_WIFSIGNALED(status), 0);

    // Every thread of the process is held, and the stop is reported once
    ktest_expect(counters_frozen(), 1);
    ktest_expect(wait_for(child, __k_WUNTRACED | __k_WNOHANG, &status), 0);

    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGCONT), 0);
    ktest_expect(wait_for(child, __k_WCONTINUED, &status), child);
    ktest_expect(__k_WIFCONTINUED(status), 1);
    ktest_expect(counters_moving(), 1);
    ktest_expect(wait_for(child, __k_WCONTINUED | __k_WNOHANG, &status), 0);

    // A process that stops again reports again
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGSTOP), 0);
    ktest_expect(wait_for(child, __k_WUNTRACED, &status), child);
    ktest_expect(counters_frozen(), 1);

    // SIGKILL reaches a stopped process
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL), 0);
    ktest_expect(wait_for(child, 0, &status), child);
    ktest_expect(__k_WIFSIGNALED(status), 1);
    ktest_expect(__k_WTERMSIG(status), __k_SIGKILL);
}

static void test_default_actions(void)
{
    int status = 0;

    // The other stop signals stop a process that has no handler for them, unless its group is orphaned, as the group
    // of the tests is: with a group of its own, the child has its parent outside of it
    long child = start_spinner(false);

    ktest_expect(ktest_syscall2(__k_SYS_SETPGID, child, child), 0);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGTSTP), 0);
    ktest_expect(wait_for(child, __k_WUNTRACED, &status), child);
    ktest_expect(__k_WSTOPSIG(status), __k_SIGTSTP);
    ktest_expect(counters_frozen(), 1);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL), 0);
    ktest_expect(wait_for(child, 0, &status), child);

    // A handler for SIGTSTP runs instead, and the process goes on
    counters->main_thread = 0;
    counters->handled = 0;
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        install(__k_SIGTSTP, &on_signal, 0);
        spin_main(false);
    }

    sleep_for(SETTLE_NSEC);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGTSTP), 0);
    sleep_for(SETTLE_NSEC);
    ktest_expect(counters->handled, 1);
    ktest_expect(counters_moving(), 1);
    ktest_expect(wait_for(child, __k_WUNTRACED | __k_WNOHANG, &status), 0);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL), 0);
    ktest_expect(wait_for(child, 0, &status), child);

    // SIGCONT with a handler still continues the process, and runs the handler
    counters->main_thread = 0;
    counters->handled = 0;
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        install(__k_SIGCONT, &on_signal, 0);
        spin_main(false);
    }

    while(counters->main_thread == 0) {
        ktest_syscall1(__k_SYS_YIELD, 0);
    }

    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGSTOP), 0);
    ktest_expect(wait_for(child, __k_WUNTRACED, &status), child);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGCONT), 0);
    ktest_expect(wait_for(child, __k_WCONTINUED, &status), child);
    sleep_for(SETTLE_NSEC);
    ktest_expect(counters->handled, 1);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL), 0);
    ktest_expect(wait_for(child, 0, &status), child);
}

static void test_parent_notice(void)
{
    int status = 0;

    // SIGCHLD with the report of the stop, unless the parent asked not to get it
    install(__k_SIGCHLD, &on_signal, 0);
    block(__k_SIGCHLD);

    long child = start_spinner(false);

    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGSTOP), 0);
    ktest_expect(wait_for(child, __k_WUNTRACED, &status), child);
    sleep_for(SETTLE_NSEC / 4); // The signal follows the wakeup of the waiter
    ktest_expect(pending() & BIT(__k_SIGCHLD), BIT(__k_SIGCHLD));

    __k_sigset_t cleared = BIT(__k_SIGCHLD);
    ktest_syscall3(__k_SYS_SIGPROCMASK, __k_SIG_UNBLOCK, (long)&cleared, 0);
    sleep_for(SETTLE_NSEC / 4);
    ktest_expect(pending() & BIT(__k_SIGCHLD), 0);

    install(__k_SIGCHLD, &on_signal, __k_SA_NOCLDSTOP);
    block(__k_SIGCHLD);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGCONT), 0);
    ktest_expect(wait_for(child, __k_WCONTINUED, &status), child);
    sleep_for(SETTLE_NSEC / 4);
    ktest_expect(pending() & BIT(__k_SIGCHLD), 0);

    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL), 0);
    ktest_expect(wait_for(child, 0, &status), child);

    // The exit of the child still sends it
    sleep_for(SETTLE_NSEC / 4);
    ktest_expect(pending() & BIT(__k_SIGCHLD), BIT(__k_SIGCHLD));
}

static void test_sleep_survives_stop(void)
{
    int status = 0;
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_timespec request;
        request.tv_sec = 0;
        request.tv_nsec = CHILD_SLEEP;

        long retval = ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)&request, 0);

        ktest_syscall1(__k_SYS_EXIT, retval == 0 ? 7 : 6);
    }

    sleep_for(SETTLE_NSEC / 2);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGSTOP), 0);
    ktest_expect(wait_for(child, __k_WUNTRACED, &status), child);

    // The sleep ends while the child is stopped, and the call returns once it runs again
    sleep_for(CHILD_SLEEP + SETTLE_NSEC);
    ktest_expect(wait_for(child, __k_WNOHANG, &status), 0);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGCONT), 0);
    ktest_expect(wait_for(child, 0, &status), child);
    ktest_expect(status, __k_W_EXITCODE(7, 0));
}

// A group that loses its last link to the rest of its session while a member is stopped gets SIGHUP and SIGCONT,
// and the signals of a terminal no longer stop its members. The link is a process that exits or leaves for a session of its own
static void test_orphaned_group(bool leaves_session)
{
    counters->hangups = 0;
    counters->continues = 0;
    counters->after_stop = 0;

    int status = 0;
    long middle = ktest_syscall1(__k_SYS_FORK, 0);

    if(middle == 0) {
        long grandchild = ktest_syscall1(__k_SYS_FORK, 0);

        if(grandchild == 0) {
            ktest_syscall2(__k_SYS_SETPGID, 0, 0);
            install(__k_SIGHUP, &on_hangup, 0);
            install(__k_SIGCONT, &on_continue, 0);

            long pid = ktest_syscall1(__k_SYS_GETPID, 0);

            ktest_syscall2(__k_SYS_KILL, pid, __k_SIGSTOP);

            // It runs again because of the SIGCONT of the exit, and is orphaned now
            ktest_syscall2(__k_SYS_KILL, pid, __k_SIGTSTP);
            counters->after_stop = 1;
            ktest_syscall1(__k_SYS_EXIT, 0);
        }

        // The grandchild is in a group of its own, and this process is its only link to the session
        wait_for(grandchild, __k_WUNTRACED, &status);

        if(leaves_session) {
            ktest_syscall1(__k_SYS_SETSID, 0);
            wait_for(grandchild, 0, &status);
        }

        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    wait_for(middle, 0, &status);

    for(int round = 0; round < 100 && counters->after_stop == 0; round += 1) {
        sleep_for(SETTLE_NSEC / 4);
    }

    ktest_expect(counters->after_stop, 1);
    ktest_expect(counters->hangups, 1);
    ktest_expect(counters->continues, 1);
}

int main(void)
{
    long address = ktest_syscall6(__k_SYS_MMAP, 0, 4096, SHARED_RW, SHARED_ANON, -1, 0);

    ktest_assert(address > 0);
    counters = (struct counters*)(address);

    test_stop_and_continue();

    counters->main_thread = 0;
    counters->other_thread = 0;
    test_default_actions();
    test_parent_notice();
    test_sleep_survives_stop();
    test_orphaned_group(false);
    test_orphaned_group(true);

    return ktest_result();
}
