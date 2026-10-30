#include <vex/errno.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define MSEC_NSEC  1000000L
#define BIT(sig)   (1UL << ((sig) - 1))
#define LONG_NSEC  5000000000L
#define POLL_COUNT 100

static volatile int alarms;

static void on_alarm(__k_int32_t sig)
{
    alarms += 1;
}

static void install(int sig, void (*handler)(__k_int32_t))
{
    struct __k_sigaction action = {};
    action.sa_handler = handler;
    action.sa_flags = __k_SA_RESTORER;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_syscall3(__k_SYS_SIGACTION, sig, (long)&action, 0);
}

static void set_mask(int how, __k_sigset_t mask)
{
    ktest_syscall3(__k_SYS_SIGPROCMASK, how, (long)&mask, 0);
}

static long now_nsec(void)
{
    struct __k_timespec now;

    ktest_syscall2(__k_SYS_GETTIME, __k_CLOCK_MONOTONIC, (long)&now);

    return now.tv_sec * 1000000000L + now.tv_nsec;
}

static struct __k_itimerspec spec(long value_nsec, long interval_nsec)
{
    struct __k_itimerspec result;
    result.it_value.tv_sec = value_nsec / 1000000000L;
    result.it_value.tv_nsec = value_nsec % 1000000000L;
    result.it_interval.tv_sec = interval_nsec / 1000000000L;
    result.it_interval.tv_nsec = interval_nsec % 1000000000L;

    return result;
}

static long arm(long value_nsec, long interval_nsec, struct __k_itimerspec* old)
{
    struct __k_itimerspec request = spec(value_nsec, interval_nsec);

    return ktest_syscall3(__k_SYS_SETITIMER, __k_ITIMER_REAL, (long)&request, (long)old);
}

static long read_timer(struct __k_itimerspec* out)
{
    return ktest_syscall3(__k_SYS_SETITIMER, __k_ITIMER_REAL, 0, (long)out);
}

// Waits for the signal with the mask open, so the handler runs
static long pause_with_mask(__k_sigset_t mask)
{
    return ktest_syscall1(__k_SYS_SIGSUSPEND, (long)&mask);
}

int main(void)
{
    install(__k_SIGALRM, &on_alarm);

    // Nothing is armed at the start
    struct __k_itimerspec state = {};

    ktest_expect(read_timer(&state), 0);
    ktest_expect(state.it_value.tv_sec + state.it_value.tv_nsec, 0);
    ktest_expect(state.it_interval.tv_sec + state.it_interval.tv_nsec, 0);

    // A one-shot timer ends sigsuspend with SIGALRM after at least its time
    set_mask(__k_SIG_SETMASK, BIT(__k_SIGALRM));

    long start = now_nsec();

    ktest_expect(arm(30 * MSEC_NSEC, 0, nullptr), 0);
    ktest_expect(read_timer(&state), 0);
    ktest_expect(state.it_value.tv_sec, 0);
    ktest_expect(state.it_value.tv_nsec > 0 && state.it_value.tv_nsec <= 30 * MSEC_NSEC, 1);
    ktest_expect(pause_with_mask(0), -__k_EINTR);
    ktest_expect(now_nsec() - start >= 30 * MSEC_NSEC, 1);
    ktest_expect(alarms, 1);

    // It is off after it fired
    ktest_expect(read_timer(&state), 0);
    ktest_expect(state.it_value.tv_sec + state.it_value.tv_nsec, 0);

    // A blocked SIGALRM stays pending
    set_mask(__k_SIG_SETMASK, BIT(__k_SIGALRM));
    ktest_expect(arm(10 * MSEC_NSEC, 0, nullptr), 0);

    __k_sigset_t pending = 0;

    for(int i = 0; i < POLL_COUNT && pending == 0; ++i) {
        struct __k_timespec nap = {};
        nap.tv_nsec = 10 * MSEC_NSEC;
        ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)&nap, 0);
        ktest_syscall1(__k_SYS_SIGPENDING, (long)&pending);
    }

    ktest_expect(pending, BIT(__k_SIGALRM));
    ktest_expect(alarms, 1);
    ktest_expect(pause_with_mask(0), -__k_EINTR); // The pending signal runs its handler now
    ktest_expect(alarms, 2);

    // An interval timer goes on until it is turned off
    alarms = 0;
    ktest_expect(arm(20 * MSEC_NSEC, 20 * MSEC_NSEC, nullptr), 0);
    ktest_expect(read_timer(&state), 0);
    ktest_expect(state.it_interval.tv_nsec, 20 * MSEC_NSEC);

    for(int i = 0; i < 3; ++i) {
        ktest_expect(pause_with_mask(0), -__k_EINTR);
    }

    ktest_expect(alarms >= 3, 1);

    // Setting returns the old state, and a zero value turns the timer off
    ktest_expect(arm(LONG_NSEC, 0, &state), 0);
    ktest_expect(state.it_interval.tv_nsec, 20 * MSEC_NSEC);
    ktest_expect(arm(0, 0, &state), 0);
    ktest_expect(state.it_value.tv_sec >= 4, 1);
    ktest_expect(state.it_interval.tv_sec + state.it_interval.tv_nsec, 0);

    alarms = 0;
    ktest_expect(read_timer(&state), 0);
    ktest_expect(state.it_value.tv_sec + state.it_value.tv_nsec, 0);

    // A fork does not inherit the timer
    ktest_expect(arm(LONG_NSEC, 0, nullptr), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_itimerspec inherited = {};

        read_timer(&inherited);
        ktest_syscall1(__k_SYS_EXIT, inherited.it_value.tv_sec + inherited.it_value.tv_nsec ? 1 : 0);
    }

    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);
    ktest_expect((wstatus >> 8) & 0xFF, 0);
    ktest_expect(arm(0, 0, nullptr), 0);

    // The default action of SIGALRM ends a process
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_sigaction action = {};

        set_mask(__k_SIG_SETMASK, 0);

        ktest_syscall3(__k_SYS_SIGACTION, __k_SIGALRM, (long)&action, 0);
        arm(10 * MSEC_NSEC, 0, nullptr);

        for(;;) {
            ktest_syscall1(__k_SYS_YIELD, 0);
        }
    }

    wstatus = -1;
    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);
    ktest_expect(wstatus & 0x7F, __k_SIGALRM);

    // Bad arguments
    struct __k_itimerspec bad = spec(0, 0);
    bad.it_value.tv_nsec = 1000000000L;

    ktest_expect(ktest_syscall3(__k_SYS_SETITIMER, 1, 0, 0), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_SETITIMER, __k_ITIMER_REAL, (long)&bad, 0), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_SETITIMER, __k_ITIMER_REAL, -4096L, 0), -__k_EFAULT);
    ktest_expect(ktest_syscall3(__k_SYS_SETITIMER, __k_ITIMER_REAL, 0, -4096L), -__k_EFAULT);

    return ktest_result();
}
