#include <vex/errno.h>
#include <vex/resource.h>
#include <vex/signal.h>
#include <vex/syscall.h>

#include "ktest.h"

#define BIT(sig)       (1UL << ((sig) - 1))
#define RT_FIRST       __k_SIGRTMIN
#define RT_SECOND      (__k_SIGRTMIN + 1)
#define RT_IGNORED     (__k_SIGRTMIN + 2)
#define MAX_HANDLED    8
#define LOW_LIMIT      4
#define HIGH_LIMIT     128
#define MISSING_PID    (1L << 28)
#define INVALID_SIGNAL (__k_NSIG + 1)

static volatile int handled_count;
static volatile long handled_values[MAX_HANDLED];
static volatile int handled_codes[MAX_HANDLED];

static void set_mask(int how, __k_sigset_t mask)
{
    ktest_syscall3(__k_SYS_SIGPROCMASK, how, (long)&mask, 0);
}

static void on_signal(__k_int32_t sig, struct __k_siginfo* info, void* context)
{
    if(handled_count < MAX_HANDLED) {
        handled_values[handled_count] = (long)(info->si_value.sival_ptr);
        handled_codes[handled_count] = info->si_code;
    }

    handled_count += 1;
}

static void install(int sig, void (*handler)(__k_int32_t, struct __k_siginfo*, void*))
{
    struct __k_sigaction action = {};
    action.sa_handler = (__k_sighandler_t)(handler);
    action.sa_flags = __k_SA_RESTORER | __k_SA_SIGINFO;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_syscall3(__k_SYS_SIGACTION, sig, (long)&action, 0);
}

static void ignore(int sig)
{
    struct __k_sigaction action = {};
    action.sa_handler = __k_SIG_IGN;
    ktest_syscall3(__k_SYS_SIGACTION, sig, (long)&action, 0);
}

// A zero timeout only looks at what is pending
static long take(__k_sigset_t set, struct __k_siginfo* info)
{
    struct __k_timespec timeout = {};

    return ktest_syscall3(__k_SYS_SIGTIMEDWAIT, (long)&set, (long)info, (long)&timeout);
}

static long queue(long pid, int sig, long value)
{
    return ktest_syscall3(__k_SYS_SIGQUEUE, pid, sig, value);
}

static void limit_pending(unsigned long current)
{
    struct __k_rlimit limit = {};
    limit.rlim_cur = current;
    limit.rlim_max = HIGH_LIMIT;

    ktest_expect(ktest_syscall2(__k_SYS_SETRLIMIT, __k_RLIMIT_SIGPENDING, (long)&limit), 0);
}

static __k_sigset_t pending_set(void)
{
    __k_sigset_t pending = 0;

    ktest_syscall1(__k_SYS_SIGPENDING, (long)&pending);

    return pending;
}

int main(void)
{
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);
    struct __k_siginfo info = {};

    set_mask(__k_SIG_SETMASK, BIT(RT_FIRST) | BIT(RT_SECOND) | BIT(__k_SIGUSR1));

    // Real-time signals queue one entry per call, the lowest number comes first, and one number keeps the order of sending
    ktest_expect(queue(pid, RT_SECOND, 20), 0);
    ktest_expect(queue(pid, RT_FIRST, 1), 0);
    ktest_expect(queue(pid, RT_FIRST, 2), 0);
    ktest_expect(queue(pid, RT_FIRST, 3), 0);

    __k_sigset_t both = BIT(RT_FIRST) | BIT(RT_SECOND);

    for(long value = 1; value <= 3; value += 1) {
        ktest_expect(take(both, &info), RT_FIRST);
        ktest_expect(info.si_code, __k_SI_QUEUE);
        ktest_expect(info.si_pid, pid);
        ktest_expect((long)(info.si_value.sival_ptr), value);

        // The bit stays set while the queue holds more of the number
        if(value < 3) {
            ktest_expect(pending_set() & BIT(RT_FIRST), BIT(RT_FIRST));
        }
    }

    ktest_expect(pending_set(), BIT(RT_SECOND));
    ktest_expect(take(both, &info), RT_SECOND);
    ktest_expect((long)(info.si_value.sival_ptr), 20);
    ktest_expect(pending_set(), 0);
    ktest_expect(take(both, nullptr), -__k_EAGAIN);

    // The whole register comes back, not only the low half
    ktest_expect(queue(pid, RT_FIRST, 0x123456789abcdefL), 0);
    ktest_expect(take(both, &info), RT_FIRST);
    ktest_expect((long)(info.si_value.sival_ptr), 0x123456789abcdefL);

    // A standard signal has no queue: the second call fails, and a taken one makes room
    ktest_expect(queue(pid, __k_SIGUSR1, 5), 0);
    ktest_expect(queue(pid, __k_SIGUSR1, 6), -__k_EAGAIN);
    ktest_expect(take(BIT(__k_SIGUSR1), &info), __k_SIGUSR1);
    ktest_expect((long)(info.si_value.sival_ptr), 5);
    ktest_expect(queue(pid, __k_SIGUSR1, 7), 0);
    ktest_expect(take(BIT(__k_SIGUSR1), &info), __k_SIGUSR1);
    ktest_expect((long)(info.si_value.sival_ptr), 7);

    // kill queues a real-time signal too, and merges a standard one
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, RT_FIRST), 0);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, RT_FIRST), 0);
    ktest_expect(take(both, &info), RT_FIRST);
    ktest_expect(info.si_code, __k_SI_USER);
    ktest_expect(take(both, nullptr), RT_FIRST);
    ktest_expect(take(both, nullptr), -__k_EAGAIN);

    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, __k_SIGUSR1), 0);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, __k_SIGUSR1), 0);
    ktest_expect(take(BIT(__k_SIGUSR1), nullptr), __k_SIGUSR1);
    ktest_expect(take(BIT(__k_SIGUSR1), nullptr), -__k_EAGAIN);

    // A handler runs once per entry, with the value of each, when the signal is unblocked
    install(RT_FIRST, &on_signal);
    ktest_expect(queue(pid, RT_FIRST, 11), 0);
    ktest_expect(queue(pid, RT_FIRST, 12), 0);
    ktest_expect(queue(pid, RT_FIRST, 13), 0);
    ktest_expect(handled_count, 0);

    set_mask(__k_SIG_UNBLOCK, BIT(RT_FIRST));

    ktest_expect(handled_count, 3);
    ktest_expect(handled_values[0], 11);
    ktest_expect(handled_values[1], 12);
    ktest_expect(handled_values[2], 13);
    ktest_expect(handled_codes[0], __k_SI_QUEUE);
    ktest_expect(pending_set(), 0);

    set_mask(__k_SIG_BLOCK, BIT(RT_FIRST));

    // The queue of a process holds RLIMIT_SIGPENDING entries for sigqueue, which refuses the next one.
    // kill goes past the limit with an entry of its own, as in Linux
    limit_pending(LOW_LIMIT);

    for(long value = 1; value <= LOW_LIMIT; value += 1) {
        ktest_expect(queue(pid, RT_FIRST, value), 0);
    }

    ktest_expect(queue(pid, RT_FIRST, 99), -__k_EAGAIN);
    ktest_expect(queue(pid, RT_SECOND, 99), -__k_EAGAIN);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, pid, RT_SECOND), 0);

    for(long value = 1; value <= LOW_LIMIT; value += 1) {
        ktest_expect(take(both, &info), RT_FIRST);
        ktest_expect((long)(info.si_value.sival_ptr), value);
    }

    ktest_expect(take(both, &info), RT_SECOND);
    ktest_expect(info.si_code, __k_SI_USER);
    ktest_expect(take(both, nullptr), -__k_EAGAIN);

    // Taking entries makes room again
    ktest_expect(queue(pid, RT_FIRST, 1), 0);
    ktest_expect(take(both, nullptr), RT_FIRST);

    // A soft limit above the hard one is refused
    struct __k_rlimit bad = {};
    bad.rlim_cur = HIGH_LIMIT + 1;
    bad.rlim_max = HIGH_LIMIT;

    ktest_expect(ktest_syscall2(__k_SYS_SETRLIMIT, __k_RLIMIT_SIGPENDING, (long)&bad), -__k_EINVAL);

    limit_pending(HIGH_LIMIT);

    // An ignored signal that a thread could take is dropped, and sigqueue says that it worked
    ignore(RT_IGNORED);
    set_mask(__k_SIG_UNBLOCK, BIT(RT_IGNORED));
    ktest_expect(queue(pid, RT_IGNORED, 1), 0);
    ktest_expect(take(BIT(RT_IGNORED), nullptr), -__k_EAGAIN);

    // Signal zero only checks the process
    ktest_expect(queue(pid, 0, 0), 0);
    ktest_expect(pending_set(), 0);

    // Bad arguments
    ktest_expect(queue(pid, -1, 0), -__k_EINVAL);
    ktest_expect(queue(pid, INVALID_SIGNAL, 0), -__k_EINVAL);
    ktest_expect(queue(0, RT_FIRST, 0), -__k_EINVAL);
    ktest_expect(queue(-pid, RT_FIRST, 0), -__k_EINVAL);
    ktest_expect(queue(MISSING_PID, RT_FIRST, 0), -__k_ESRCH);

    return ktest_result();
}
