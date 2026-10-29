#include <vex/capability.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/pgrp.h>
#include <vex/process.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>

#include "syscall_internal.h"

#define SA_KNOWN (SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO | SA_ONSTACK | SA_RESTART | SA_NODEFER | SA_RESETHAND | SA_RESTORER)

typedef struct group_kill_s group_kill_t;

struct group_kill_s {
    thread_t* thread;
    int sig;
    int retval; // -EPERM until one member may be signalled, then the first failure if any
};

static bool may_signal(const credentials_t* restrict sender, const credentials_t* restrict target)
{
    if(cred_capable(sender, CAP_KILL)) {
        return true;
    }

    return sender->uid == target->uid || sender->uid == target->suid || sender->euid == target->uid || sender->euid == target->suid;
}

static int kill_process(thread_t* restrict thread, process_t* restrict target, int sig)
{
    credentials_t* target_cred = process_get_cred(target);
    bool allowed = may_signal(thread->credentials, target_cred);
    cred_decref(target_cred);

    if(!allowed) {
        return -EPERM;
    }

    if(sig == 0) {
        return 0;
    }

    process_t* sender = thread->process;

    siginfo_t info = {};
    info.si_signo = sig;
    info.si_code = SI_USER;
    info.si_pid = sender->pid;
    info.si_uid = thread->credentials->uid;

    return signal_send(target, 0, &info);
}

static void kill_member(process_t* restrict process, void* restrict arg)
{
    group_kill_t* kill = arg;
    int retval = kill_process(kill->thread, process, kill->sig);

    if(retval == -EPERM) {
        return;
    }

    if(kill->retval == -EPERM || kill->retval == 0) {
        kill->retval = retval;
    }
}

static intptr_t kill_group(thread_t* restrict thread, pid_t pgid, int sig)
{
    group_kill_t kill = { 0 };
    kill.thread = thread;
    kill.sig = sig;
    kill.retval = -EPERM;

    int retval = process_group_for_each(pgid, &kill_member, &kill);

    if(retval < 0) {
        return retval;
    }

    return kill.retval;
}

static intptr_t sys_kill(pid_t pid, int sig)
{
    thread_t* thread = this_thread();
    process_t* sender = thread->process;

    if(sender == nullptr) {
        return -ESRCH;
    }

    if(sig < 0 || sig > NSIG) {
        return -EINVAL;
    }

    if(pid == -1) {
        return -EINVAL; // UNDONE: every process the sender may signal
    }

    if(pid == INT32_MIN) {
        return -ESRCH; // No group has the negated id
    }

    if(pid == 0) {
        pid_t pgid = process_get_pgid(sender);
        return kill_group(thread, pgid, sig);
    }

    if(pid < 0) {
        return kill_group(thread, -pid, sig);
    }

    process_t* target = process_locate(pid);

    if(target == nullptr) {
        return -ESRCH;
    }

    intptr_t retval = kill_process(thread, target, sig);
    process_decref(target);

    return retval;
}

define_syscall(sys_kill, SYS_KILL, 2);

static intptr_t sys_tgkill(pid_t pid, pid_t tid, int sig)
{
    thread_t* thread = this_thread();
    process_t* sender = thread->process;

    if(sender == nullptr) {
        return -ESRCH;
    }

    if(sig < 0 || sig > NSIG || pid <= 0 || tid <= 0) {
        return -EINVAL;
    }

    process_t* target = process_locate(pid);

    if(target == nullptr) {
        return -ESRCH;
    }

    credentials_t* target_cred = process_get_cred(target);
    bool allowed = may_signal(thread->credentials, target_cred);
    cred_decref(target_cred);

    intptr_t retval = -EPERM;

    if(allowed && sig == 0) {
        retval = -ESRCH;

        if(process_has_thread(target, tid)) {
            retval = 0;
        }
    }
    else if(allowed) {
        siginfo_t info = {};
        info.si_signo = sig;
        info.si_code = SI_TKILL;
        info.si_pid = sender->pid;
        info.si_uid = thread->credentials->uid;

        retval = signal_send(target, tid, &info);
    }

    process_decref(target);

    return retval;
}

define_syscall(sys_tgkill, SYS_TGKILL, 3);

static intptr_t sys_sigaction(int sig, uintptr_t user_act, uintptr_t user_oact)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(sig < 1 || sig > NSIG) {
        return -EINVAL;
    }

    sigaction_t act;
    sigaction_t old_action;

    if(user_act) {
        ssize_t copied = user_copyin(&act, user_act, sizeof(act));

        if(copied < 0) {
            return copied;
        }

        if(sig == SIGKILL || sig == SIGSTOP || (act.sa_flags & ~SA_KNOWN)) {
            return -EINVAL;
        }

        // There is no trampoline of the kernel's own to return through
        if(act.sa_handler != SIG_DFL && act.sa_handler != SIG_IGN && 0 == (act.sa_flags & SA_RESTORER)) {
            return -EINVAL;
        }

        act.sa_mask &= ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
    }

    signal_action(process, sig, user_act ? &act : nullptr, &old_action);

    if(user_oact) {
        ssize_t copied = user_copyout(user_oact, &old_action, sizeof(old_action));

        if(copied < 0) {
            return copied;
        }
    }

    return 0;
}

define_syscall(sys_sigaction, SYS_SIGACTION, 3);

static intptr_t sys_sigprocmask(int how, uintptr_t user_set, uintptr_t user_oset)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    sigset_t set = 0;

    if(user_set) {
        if(how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
            return -EINVAL;
        }

        ssize_t copied = user_copyin(&set, user_set, sizeof(set));

        if(copied < 0) {
            return copied;
        }
    }
    else {
        how = SIG_BLOCK; // Blocking nothing more only reads the mask
    }

    sigset_t oset = signal_mask(thread, how, set);

    if(user_oset) {
        ssize_t copied = user_copyout(user_oset, &oset, sizeof(oset));

        if(copied < 0) {
            return copied;
        }
    }

    return 0;
}

define_syscall(sys_sigprocmask, SYS_SIGPROCMASK, 3);

static intptr_t sys_sigpending(uintptr_t user_set)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    sigset_t pending = signal_pending(thread);
    ssize_t copied = user_copyout(user_set, &pending, sizeof(pending));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_sigpending, SYS_SIGPENDING, 1);

static intptr_t sys_sigreturn(void)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    hart_context_t* ctx = thread->syscall_ctx;
    sigset_t mask;

    if(arch_signal_return(thread, ctx, &mask) < 0) {
        process_kill(process, SIGSEGV);
        return 0;
    }

    signal_mask(thread, SIG_SETMASK, mask);

    // The return value lands in the register just restored
    return ctx->syscall_retval;
}

define_syscall(sys_sigreturn, SYS_SIGRETURN, 0);

static intptr_t sys_sigaltstack(uintptr_t user_ss, uintptr_t user_old)
{
    thread_t* thread = this_thread();

    if(thread->process == nullptr) {
        return -ENOSYS;
    }

    hart_context_t* ctx = thread->syscall_ctx;

    stack_t old;
    signal_altstack_get(thread, ctx->syscall_sp, &old);

    if(user_ss) {
        stack_t ss;
        ssize_t copied = user_copyin(&ss, user_ss, sizeof(ss));

        if(copied < 0) {
            return copied;
        }

        if(old.ss_flags & SS_ONSTACK) {
            return -EPERM;
        }

        if(ss.ss_flags & ~SS_DISABLE) {
            return -EINVAL;
        }

        if(ss.ss_flags & SS_DISABLE) {
            thread->sig_altstack = 0;
            thread->sig_altstack_size = 0;
        }
        else {
            uintptr_t base = (uintptr_t)(ss.ss_sp);

            if(ss.ss_size < MINSIGSTKSZ || ss.ss_size > UINT32_MAX) {
                return -ENOMEM;
            }

            if(base + ss.ss_size < base) {
                return -EINVAL;
            }

            thread->sig_altstack = base;
            thread->sig_altstack_size = ss.ss_size;
        }
    }

    if(user_old) {
        ssize_t copied = user_copyout(user_old, &old, sizeof(old));

        if(copied < 0) {
            return copied;
        }
    }

    return 0;
}

define_syscall(sys_sigaltstack, SYS_SIGALTSTACK, 2);
