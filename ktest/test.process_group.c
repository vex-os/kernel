#include <vex/errno.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define BROADCAST_WAIT_NSEC 10000000L

static volatile unsigned int never_changes;
static volatile int broadcast_seen;

static void on_broadcast(__k_int32_t sig)
{
    broadcast_seen = 1;
}

static int run(void)
{
    int wstatus;
    long child;
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);

    // A session leader can neither start a session nor leave its group
    ktest_expect(ktest_syscall1(__k_SYS_GETPGID, 0), pid);
    ktest_expect(ktest_syscall1(__k_SYS_GETSID, 0), pid);
    ktest_expect(ktest_syscall1(__k_SYS_SETSID, 0), -__k_EPERM);
    ktest_expect(ktest_syscall2(__k_SYS_SETPGID, 0, 0), -__k_EPERM);
    ktest_expect(ktest_syscall1(__k_SYS_GETPGID, 99999), -__k_ESRCH);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, 0, 0), 0); // its own group

    // Both the shell and the job set the job's group, whichever runs first
    long leader = ktest_syscall1(__k_SYS_FORK, 0);

    if(leader == 0) {
        ktest_syscall2(__k_SYS_SETPGID, 0, 0);

        for(;;) {
            ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, 0, 0);
        }
    }

    long member = ktest_syscall1(__k_SYS_FORK, 0);

    if(member == 0) {
        for(;;) {
            ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, 0, 0);
        }
    }

    ktest_expect(ktest_syscall2(__k_SYS_SETPGID, leader, leader), 0);
    ktest_expect(ktest_syscall2(__k_SYS_SETPGID, member, leader), 0);
    ktest_expect(ktest_syscall1(__k_SYS_GETPGID, member) == leader, 1);
    ktest_expect(ktest_syscall2(__k_SYS_SETPGID, member, 99999), -__k_EPERM); // no such group
    ktest_expect(ktest_syscall2(__k_SYS_SETPGID, 99999, leader), -__k_ESRCH);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, -leader, __k_SIGKILL), 0);

    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, -leader, (long)&wstatus, 0) > 0, 1);
    ktest_expect(wstatus, 9);
    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, -leader, (long)&wstatus, 0) > 0, 1);
    ktest_expect(wstatus, 9);
    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, -leader, (long)&wstatus, 0), -__k_ECHILD); // group gone
    ktest_expect(ktest_syscall2(__k_SYS_KILL, -leader, 0), -__k_ESRCH);

    // kill(-1) reaches every process but the sender; the parent of this test only ignores the signal
    ktest_expect(ktest_syscall2(__k_SYS_KILL, -1, 0), 0);

    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_sigaction action = {};
        action.sa_handler = &on_broadcast;
        action.sa_flags = __k_SA_RESTORER;
        action.sa_restorer = &ktest_signal_restorer;
        ktest_syscall3(__k_SYS_SIGACTION, __k_SIGWINCH, (long)&action, 0);

        struct __k_timespec timeout = {};
        timeout.tv_nsec = BROADCAST_WAIT_NSEC;

        while(!broadcast_seen) {
            ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, (long)&timeout, 0);
        }

        ktest_syscall1(__k_SYS_EXIT, 7);
    }

    // Sent until it lands, as a signal before the handler exists is dropped
    long reaped = 0;

    while(reaped == 0) {
        ktest_expect(ktest_syscall2(__k_SYS_KILL, -1, __k_SIGWINCH), 0);
        ktest_syscall1(__k_SYS_YIELD, 0);
        reaped = ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, __k_WNOHANG);
    }

    ktest_expect(reaped, child);
    ktest_expect(wstatus, __k_W_EXITCODE(7, 0));

    // A session leader stays in its own group, and its session is out of the parent's reach
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        long own = ktest_syscall1(__k_SYS_GETPID, 0);
        int code = 0;

        if(ktest_syscall1(__k_SYS_SETSID, 0) == own && ktest_syscall1(__k_SYS_GETSID, 0) == own
            && ktest_syscall1(__k_SYS_GETPGID, 0) == own) {
            code += 1;
        }

        if(ktest_syscall2(__k_SYS_SETPGID, 0, 1) == -__k_EPERM) {
            code += 2;
        }

        if(ktest_syscall1(__k_SYS_SETSID, 0) == -__k_EPERM) {
            code += 4;
        }

        ktest_syscall1(__k_SYS_EXIT, code);
    }

    wstatus = -1;
    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);
    ktest_expect(wstatus, 1792);

    // A child left in the caller's group is what waitpid(0) waits for
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        ktest_syscall1(__k_SYS_EXIT, 4);
    }

    wstatus = -1;
    ktest_expect(ktest_syscall3(__k_SYS_WAITPID, 0, (long)&wstatus, 0) == child, 1);
    ktest_expect(wstatus, 1024);

    return ktest_result();
}

int main(void)
{
    return ktest_in_new_session(&run);
}
