#include <vex/errno.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define SETTLE_NSEC  50000000L
#define TIMEOUT_NSEC 30000000L
#define LONG_NSEC    2000000000L
#define BIT(sig)     (1UL << ((sig) - 1))

static volatile int handled_usr1;
static volatile int handled_usr2;

static __k_sigset_t get_mask(void)
{
    __k_sigset_t mask = 0;

    ktest_syscall3(__k_SYS_SIGPROCMASK, __k_SIG_BLOCK, 0, (long)&mask);

    return mask;
}

static void set_mask(int how, __k_sigset_t mask)
{
    ktest_syscall3(__k_SYS_SIGPROCMASK, how, (long)&mask, 0);
}

static void on_usr1(__k_int32_t sig)
{
    handled_usr1 += 1;
}

static void on_usr2(__k_int32_t sig)
{
    handled_usr2 += 1;
}

static void install(int sig, void (*handler)(__k_int32_t))
{
    struct __k_sigaction action = {};
    action.sa_handler = handler;
    action.sa_flags = __k_SA_RESTORER;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_syscall3(__k_SYS_SIGACTION, sig, (long)&action, 0);
}

static long wait_for(__k_sigset_t set, struct __k_siginfo* info, long nanoseconds)
{
    struct __k_timespec timeout;
    timeout.tv_sec = nanoseconds / 1000000000L;
    timeout.tv_nsec = nanoseconds % 1000000000L;

    return ktest_syscall3(__k_SYS_SIGTIMEDWAIT, (long)&set, (long)info, (long)&timeout);
}

static long now_nsec(void)
{
    struct __k_timespec now;

    ktest_syscall2(__k_SYS_GETTIME, __k_CLOCK_MONOTONIC, (long)&now);

    return now.tv_sec * 1000000000L + now.tv_nsec;
}

static void sleep_for(long nanoseconds)
{
    struct __k_timespec request;
    request.tv_sec = nanoseconds / 1000000000L;
    request.tv_nsec = nanoseconds % 1000000000L;

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)&request, 0);
}

// A child that sends the signal after the parent had time to start waiting, and then exits
static long send_later(long target, int sig)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_syscall2(__k_SYS_KILL, target, sig);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    return child;
}

static void reap(long child)
{
    int wstatus = -1;

    ktest_syscall4(__k_SYS_WAITPID, child, (long)&wstatus, 0, 0);
}

int main(void)
{
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);

    install(__k_SIGUSR1, &on_usr1);
    install(__k_SIGUSR2, &on_usr2);

    // A blocked signal that is pending already is taken at once, with its siginfo, and runs no handler
    set_mask(__k_SIG_SETMASK, BIT(__k_SIGUSR1));
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, __k_SIGUSR1), 0);

    struct __k_siginfo info = {};

    ktest_expect(wait_for(BIT(__k_SIGUSR1), &info, LONG_NSEC), __k_SIGUSR1);
    ktest_expect(info.si_signo, __k_SIGUSR1);
    ktest_expect(info.si_code, __k_SI_USER);
    ktest_expect(info.si_pid, pid);
    ktest_expect(handled_usr1, 0);
    ktest_expect(get_mask(), BIT(__k_SIGUSR1));

    __k_sigset_t pending = 0;

    ktest_syscall1(__k_SYS_SIGPENDING, (long)&pending);
    ktest_expect(pending, 0);

    // Nothing pending: a zero timeout looks only, and a short one waits for as long as it says
    ktest_expect(wait_for(BIT(__k_SIGUSR1), nullptr, 0), -__k_EAGAIN);

    long start = now_nsec();

    ktest_expect(wait_for(BIT(__k_SIGUSR1), nullptr, TIMEOUT_NSEC), -__k_EAGAIN);
    ktest_expect(now_nsec() - start >= TIMEOUT_NSEC, 1);
    ktest_expect(get_mask(), BIT(__k_SIGUSR1));

    // A signal outside of the set stays pending
    set_mask(__k_SIG_SETMASK, BIT(__k_SIGUSR1) | BIT(__k_SIGUSR2));
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, __k_SIGUSR2), 0);
    ktest_expect(wait_for(BIT(__k_SIGUSR1), nullptr, 0), -__k_EAGAIN);

    ktest_syscall1(__k_SYS_SIGPENDING, (long)&pending);
    ktest_expect(pending, BIT(__k_SIGUSR2));
    ktest_expect(wait_for(BIT(__k_SIGUSR2), nullptr, 0), __k_SIGUSR2);
    ktest_expect(handled_usr2, 0);

    // A signal that arrives during the wait ends it, and the mask is the old one afterwards
    long child = send_later(pid, __k_SIGUSR1);

    ktest_expect(wait_for(BIT(__k_SIGUSR1), &info, LONG_NSEC), __k_SIGUSR1);
    reap(child);
    ktest_expect(info.si_pid, child);
    ktest_expect(handled_usr1, 0);
    ktest_expect(get_mask(), BIT(__k_SIGUSR1) | BIT(__k_SIGUSR2));

    // A signal of the set is taken even when it is not blocked and has a handler
    set_mask(__k_SIG_SETMASK, 0);
    child = send_later(pid, __k_SIGUSR1);

    ktest_expect(wait_for(BIT(__k_SIGUSR1), nullptr, LONG_NSEC), __k_SIGUSR1);
    reap(child);
    ktest_expect(handled_usr1, 0);
    ktest_expect(get_mask(), 0);

    // A signal outside of the set that has a handler ends the wait with EINTR
    child = send_later(pid, __k_SIGUSR2);

    ktest_expect(wait_for(BIT(__k_SIGUSR1), nullptr, LONG_NSEC), -__k_EINTR);
    reap(child);
    ktest_expect(handled_usr2, 1);
    ktest_expect(get_mask(), 0);

    // A signal that the set does not name and the mask blocks does not end the wait
    set_mask(__k_SIG_SETMASK, BIT(__k_SIGUSR2));
    child = send_later(pid, __k_SIGUSR2);

    start = now_nsec();

    ktest_expect(wait_for(BIT(__k_SIGUSR1), nullptr, 3 * SETTLE_NSEC), -__k_EAGAIN);
    reap(child);
    ktest_expect(now_nsec() - start >= 3 * SETTLE_NSEC, 1);
    ktest_expect(handled_usr2, 1);
    ktest_expect(get_mask(), BIT(__k_SIGUSR2));
    ktest_expect(wait_for(BIT(__k_SIGUSR2), nullptr, 0), __k_SIGUSR2);

    // The signals that cannot be caught are left out of the set
    ktest_expect(wait_for(BIT(__k_SIGKILL) | BIT(__k_SIGSTOP), nullptr, 0), -__k_EAGAIN);

    // Bad arguments
    __k_sigset_t set = BIT(__k_SIGUSR1);
    struct __k_timespec bad = {};
    bad.tv_nsec = 1000000000L;

    ktest_expect(ktest_syscall3(__k_SYS_SIGTIMEDWAIT, -4096L, 0, 0), -__k_EFAULT);
    ktest_expect(ktest_syscall3(__k_SYS_SIGTIMEDWAIT, (long)&set, 0, -4096L), -__k_EFAULT);
    ktest_expect(ktest_syscall3(__k_SYS_SIGTIMEDWAIT, (long)&set, 0, (long)&bad), -__k_EINVAL);

    set_mask(__k_SIG_SETMASK, BIT(__k_SIGUSR1) | BIT(__k_SIGUSR2));
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, __k_SIGUSR1), 0);
    ktest_expect(ktest_syscall3(__k_SYS_SIGTIMEDWAIT, (long)&set, -4096L, 0), -__k_EFAULT);
    ktest_expect(get_mask(), BIT(__k_SIGUSR1) | BIT(__k_SIGUSR2));

    return ktest_result();
}
