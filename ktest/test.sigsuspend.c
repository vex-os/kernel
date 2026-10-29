#include <vex/errno.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define SETTLE_NSEC 50000000L
#define BIT(sig)    (1UL << ((sig) - 1))

static volatile int handled_usr1;
static volatile int handled_usr2;
static volatile __k_sigset_t mask_in_handler;

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
    mask_in_handler = get_mask();
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

static long suspend(__k_sigset_t mask)
{
    return ktest_syscall1(__k_SYS_SIGSUSPEND, (long)&mask);
}

static void sleep_for(long nanoseconds)
{
    struct __k_timespec request;
    request.tv_sec = nanoseconds / 1000000000L;
    request.tv_nsec = nanoseconds % 1000000000L;

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)&request, 0);
}

// A child that sends the signals after the parent had time to start waiting, and then exits
static long send_later(long target, int first, int second)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_syscall2(__k_SYS_KILL, target, first);

        if(second) {
            ktest_syscall2(__k_SYS_KILL, target, second);
        }

        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    return child;
}

static void reap(long child)
{
    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);
}

int main(void)
{
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);

    install(__k_SIGUSR1, &on_usr1);
    install(__k_SIGUSR2, &on_usr2);

    // The signal ends the wait, its handler runs with the temporary mask, and the old mask is back afterwards
    set_mask(__k_SIG_SETMASK, BIT(__k_SIGUSR1));

    long child = send_later(pid, __k_SIGUSR1, 0);

    ktest_expect(suspend(0), -__k_EINTR);
    reap(child);
    ktest_expect(handled_usr1, 1);
    ktest_expect(mask_in_handler, BIT(__k_SIGUSR1)); // Its own signal, on top of the empty temporary mask
    ktest_expect(get_mask(), BIT(__k_SIGUSR1));

    // A signal that was pending already ends the wait at once
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, __k_SIGUSR1), 0);
    ktest_expect(handled_usr1, 1);
    ktest_expect(suspend(0), -__k_EINTR);
    ktest_expect(handled_usr1, 2);
    ktest_expect(get_mask(), BIT(__k_SIGUSR1));

    // A signal that the temporary mask blocks does not end the wait, and stays pending
    set_mask(__k_SIG_SETMASK, BIT(__k_SIGUSR1) | BIT(__k_SIGUSR2));
    child = send_later(pid, __k_SIGUSR1, __k_SIGUSR2);

    ktest_expect(suspend(BIT(__k_SIGUSR1)), -__k_EINTR);
    reap(child);
    ktest_expect(handled_usr2, 1);
    ktest_expect(handled_usr1, 2);

    __k_sigset_t pending = 0;

    ktest_syscall1(__k_SYS_SIGPENDING, (long)&pending);
    ktest_expect(pending, BIT(__k_SIGUSR1));
    ktest_expect(get_mask(), BIT(__k_SIGUSR1) | BIT(__k_SIGUSR2));

    ktest_expect(ktest_syscall1(__k_SYS_SIGSUSPEND, -4096L), -__k_EFAULT);
    ktest_expect(get_mask(), BIT(__k_SIGUSR1) | BIT(__k_SIGUSR2));

    return ktest_result();
}
