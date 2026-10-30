#include <vex/errno.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/wait.h>

#include "ktest.h"

static long wait_id(long idtype, long id, struct __k_siginfo* info, long options)
{
    return ktest_syscall5(__k_SYS_WAITID, idtype, id, (long)info, options, 0);
}

static void clear(struct __k_siginfo* info)
{
    memset(info, 0xAA, sizeof(*info));
}

static long fork_exit(long code)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        ktest_syscall1(__k_SYS_EXIT, code);
    }

    return child;
}

// A child that runs until a signal ends it
static long fork_spin(void)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        for(;;) {
            ktest_syscall1(__k_SYS_YIELD, 0);
        }
    }

    return child;
}

int main(void)
{
    struct __k_siginfo info;

    // Nothing to wait for
    ktest_expect(wait_id(__k_P_ALL, 0, &info, __k_WEXITED), -__k_ECHILD);

    // An exit, reaped by the wait
    long child = fork_exit(7);

    clear(&info);
    ktest_expect(wait_id(__k_P_PID, child, &info, __k_WEXITED), 0);
    ktest_expect(info.si_signo, __k_SIGCHLD);
    ktest_expect(info.si_code, __k_CLD_EXITED);
    ktest_expect(info.si_pid, child);
    ktest_expect(info.si_status, 7);
    ktest_expect(wait_id(__k_P_PID, child, &info, __k_WEXITED | __k_WNOHANG), -__k_ECHILD);

    // WNOWAIT leaves the zombie to waitpid
    child = fork_exit(3);

    for(int i = 0; i < 2; ++i) {
        clear(&info);
        ktest_expect(wait_id(__k_P_PID, child, &info, __k_WEXITED | __k_WNOWAIT), 0);
        ktest_expect(info.si_code, __k_CLD_EXITED);
        ktest_expect(info.si_status, 3);
    }

    int wstatus = -1;

    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)&wstatus, 0, 0), child);
    ktest_expect(__k_WEXITSTATUS(wstatus), 3);

    // WNOHANG finds nothing yet, and says so with a zero signal number and pid
    child = fork_spin();

    clear(&info);
    ktest_expect(wait_id(__k_P_ALL, 0, &info, __k_WEXITED | __k_WNOHANG), 0);
    ktest_expect(info.si_signo, 0);
    ktest_expect(info.si_pid, 0);

    // A stop is reported once, and a WNOWAIT look leaves it
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGSTOP), 0);

    for(int i = 0; i < 2; ++i) {
        clear(&info);
        ktest_expect(wait_id(__k_P_PGID, 0, &info, __k_WSTOPPED | __k_WNOWAIT), 0);
        ktest_expect(info.si_code, __k_CLD_STOPPED);
        ktest_expect(info.si_status, __k_SIGSTOP);
        ktest_expect(info.si_pid, child);
    }

    ktest_expect(wait_id(__k_P_PID, child, &info, __k_WSTOPPED), 0);
    ktest_expect(wait_id(__k_P_PID, child, &info, __k_WSTOPPED | __k_WNOHANG), 0);
    ktest_expect(info.si_pid, 0);

    // The same for a continue
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGCONT), 0);

    clear(&info);
    ktest_expect(wait_id(__k_P_PID, child, &info, __k_WCONTINUED | __k_WNOWAIT), 0);
    ktest_expect(info.si_code, __k_CLD_CONTINUED);
    ktest_expect(info.si_status, __k_SIGCONT);
    ktest_expect(wait_id(__k_P_PID, child, &info, __k_WCONTINUED), 0);
    ktest_expect(wait_id(__k_P_PID, child, &info, __k_WCONTINUED | __k_WNOHANG), 0);
    ktest_expect(info.si_pid, 0);

    // A kill is reported with the signal
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL), 0);

    clear(&info);
    ktest_expect(wait_id(__k_P_PGID, 0, &info, __k_WEXITED), 0);
    ktest_expect(info.si_code, __k_CLD_KILLED);
    ktest_expect(info.si_status, __k_SIGKILL);
    ktest_expect(info.si_pid, child);

    // Bad arguments
    child = fork_exit(0);

    ktest_expect(wait_id(__k_P_ALL, 0, &info, 0), -__k_EINVAL);
    ktest_expect(wait_id(__k_P_ALL, 0, &info, __k_WEXITED | 0x40000000), -__k_EINVAL);
    ktest_expect(wait_id(3, 0, &info, __k_WEXITED), -__k_EINVAL);
    ktest_expect(wait_id(__k_P_PID, 0, &info, __k_WEXITED), -__k_ECHILD);
    ktest_expect(wait_id(__k_P_PGID, -1, &info, __k_WEXITED), -__k_ECHILD);
    ktest_expect(wait_id(__k_P_PID, child + 1000, &info, __k_WEXITED), -__k_ECHILD);
    ktest_expect(wait_id(__k_P_PID, child, (struct __k_siginfo*)(-4096L), __k_WEXITED), -__k_EFAULT);

    // A null siginfo is allowed
    child = fork_exit(0);
    ktest_expect(wait_id(__k_P_PID, child, nullptr, __k_WEXITED), 0);

    return ktest_result();
}
