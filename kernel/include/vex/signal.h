#pragma once

#include <usr/vex/signal.h>
#include <vex/compiler.h>
#include <vex/spinlock.h>

typedef struct hart_context_s hart_context_t;
typedef struct process_s process_t;
typedef struct thread_s thread_t;

#define SIGHUP    __k_SIGHUP
#define SIGINT    __k_SIGINT
#define SIGQUIT   __k_SIGQUIT
#define SIGILL    __k_SIGILL
#define SIGTRAP   __k_SIGTRAP
#define SIGABRT   __k_SIGABRT
#define SIGBUS    __k_SIGBUS
#define SIGFPE    __k_SIGFPE
#define SIGKILL   __k_SIGKILL
#define SIGSEGV   __k_SIGSEGV
#define SIGPIPE   __k_SIGPIPE
#define SIGUSR1   __k_SIGUSR1
#define SIGUSR2   __k_SIGUSR2
#define SIGALRM   __k_SIGALRM
#define SIGTERM   __k_SIGTERM
#define SIGCHLD   __k_SIGCHLD
#define SIGCONT   __k_SIGCONT
#define SIGSTOP   __k_SIGSTOP
#define SIGTSTP   __k_SIGTSTP
#define SIGTTIN   __k_SIGTTIN
#define SIGTTOU   __k_SIGTTOU
#define SIGURG    __k_SIGURG
#define SIGXCPU   __k_SIGXCPU
#define SIGXFSZ   __k_SIGXFSZ
#define SIGVTALRM __k_SIGVTALRM
#define SIGPROF   __k_SIGPROF
#define SIGWINCH  __k_SIGWINCH
#define SIGSYS    __k_SIGSYS
#define SIGINFO   __k_SIGINFO
#define SIGIO     __k_SIGIO
#define SIGPWR    __k_SIGPWR
#define SIGRTMIN  __k_SIGRTMIN
#define SIGRTMAX  __k_SIGRTMAX
#define NSIG      __k_NSIG

#define SIG_DFL      __k_SIG_DFL
#define SIG_IGN      __k_SIG_IGN
#define SA_NOCLDSTOP __k_SA_NOCLDSTOP
#define SA_NOCLDWAIT __k_SA_NOCLDWAIT
#define SA_SIGINFO   __k_SA_SIGINFO
#define SA_ONSTACK   __k_SA_ONSTACK
#define SA_RESTART   __k_SA_RESTART
#define SA_NODEFER   __k_SA_NODEFER
#define SA_RESETHAND __k_SA_RESETHAND
#define SA_RESTORER  __k_SA_RESTORER
#define SIG_BLOCK    __k_SIG_BLOCK
#define SIG_UNBLOCK  __k_SIG_UNBLOCK
#define SIG_SETMASK  __k_SIG_SETMASK

#define SI_USER       __k_SI_USER
#define SI_QUEUE      __k_SI_QUEUE
#define SI_TIMER      __k_SI_TIMER
#define SI_MESGQ      __k_SI_MESGQ
#define SI_ASYNCIO    __k_SI_ASYNCIO
#define SI_KERNEL     __k_SI_KERNEL
#define SI_TKILL      __k_SI_TKILL
#define ILL_ILLOPC    __k_ILL_ILLOPC
#define ILL_ILLOPN    __k_ILL_ILLOPN
#define ILL_ILLADR    __k_ILL_ILLADR
#define ILL_ILLTRP    __k_ILL_ILLTRP
#define ILL_PRVOPC    __k_ILL_PRVOPC
#define ILL_PRVREG    __k_ILL_PRVREG
#define ILL_COPROC    __k_ILL_COPROC
#define ILL_BADSTK    __k_ILL_BADSTK
#define FPE_INTDIV    __k_FPE_INTDIV
#define FPE_INTOVF    __k_FPE_INTOVF
#define FPE_FLTDIV    __k_FPE_FLTDIV
#define FPE_FLTOVF    __k_FPE_FLTOVF
#define FPE_FLTUND    __k_FPE_FLTUND
#define FPE_FLTRES    __k_FPE_FLTRES
#define FPE_FLTINV    __k_FPE_FLTINV
#define FPE_FLTSUB    __k_FPE_FLTSUB
#define SEGV_MAPERR   __k_SEGV_MAPERR
#define SEGV_ACCERR   __k_SEGV_ACCERR
#define BUS_ADRALN    __k_BUS_ADRALN
#define BUS_ADRERR    __k_BUS_ADRERR
#define BUS_OBJERR    __k_BUS_OBJERR
#define TRAP_BRKPT    __k_TRAP_BRKPT
#define TRAP_TRACE    __k_TRAP_TRACE
#define CLD_EXITED    __k_CLD_EXITED
#define CLD_KILLED    __k_CLD_KILLED
#define CLD_DUMPED    __k_CLD_DUMPED
#define CLD_TRAPPED   __k_CLD_TRAPPED
#define CLD_STOPPED   __k_CLD_STOPPED
#define CLD_CONTINUED __k_CLD_CONTINUED

#define SS_ONSTACK __k_SS_ONSTACK
#define SS_DISABLE __k_SS_DISABLE
#define UC_FPU     __k_UC_FPU

typedef struct signal_actions_s signal_actions_t;
typedef struct pending_signal_s pending_signal_t;

typedef struct __k_sigaction sigaction_t;
typedef struct __k_siginfo siginfo_t;
typedef struct __k_stack stack_t;
typedef struct __k_ucontext ucontext_t;

typedef __k_sigset_t sigset_t;
typedef __k_sighandler_t sighandler_t;

struct signal_actions_s {
    spinlock_t lock;
    sigaction_t actions[NSIG];
};

struct pending_signal_s {
    pending_signal_t* next;
    siginfo_t info;
};

signal_actions_t* signal_actions_create(signal_actions_t* restrict parent) __nodiscard;
void signal_actions_destroy(signal_actions_t* restrict signal_actions);
void signal_actions_exec(signal_actions_t* restrict signal_actions);

/// Queues an asynchronous signal; SIGKILL ends the process at once, and init
/// only ever receives signals it handles
/// @param tid A thread of the process, or zero for whichever thread can take it
/// @note A standard signal that is pending already is merged with the new one. A
/// signal from SIGRTMIN up gets its own entry, past RLIMIT_SIGPENDING too, as in Linux for kill
/// @retval 0 Queued, merged with one already pending, or dropped as ignored
/// @retval -ESRCH No such thread in the process
int signal_send(process_t* restrict process, pid_t tid, const siginfo_t* restrict info);

/// Queues a signal with its siginfo for the process, as sigqueue does
/// @pre The siginfo carries the value and the sender, and its code is SI_QUEUE
/// @note A signal from SIGRTMIN up queues once per call. The queue of the process holds RLIMIT_SIGPENDING signals at most
/// @retval 0 Queued, or dropped as ignored
/// @retval -EAGAIN The limit is reached, a standard signal of this number is pending already, or there is no memory
int signal_queue(process_t* restrict process, const siginfo_t* restrict info);

/// Queues an asynchronous signal with no siginfo of its own, so that it allocates nothing and the scheduler tick can send it
/// @note The receiver gets SI_KERNEL. The same rules apply as in signal_send
void signal_raise(process_t* restrict process, int sig);

/// signal_send to the whole process for senders that cannot allocate,
/// such as the scheduler; the node is consumed either way
void signal_send_node(process_t* restrict process, pending_signal_t* restrict node);

/// Makes every thread that could take a pending process-wide signal look again
/// @pre A thread just left the process, and may have been the one chosen for such a signal
void signal_retarget(process_t* restrict process);

/// Raises a fault signal. Unblocks the signal, resets an ignored or blocked handler, and kills the process if the action is the default one
/// @pre The thread is the calling thread and has just faulted
/// @note A blocked or ignored fault signal would return to the faulting instruction forever
void signal_force(thread_t* restrict thread, const siginfo_t* restrict info);

void signal_deliver(hart_context_t* restrict ctx);

/// signal_deliver for a thread inside a system call, which can take no handler frame
/// @pre The caller is the thread
/// @retval true A signal without a handler was taken, or none was deliverable
/// @retval false The next signal has a handler; it stays pending for the way out
bool signal_deliver_uncaught(hart_context_t* restrict ctx) __nodiscard;
void signal_action(process_t* restrict process, int sig, const sigaction_t* restrict act, sigaction_t* restrict old_action);
sigset_t signal_mask(thread_t* restrict thread, int how, sigset_t set);

/// Sets the mask for the time of a wait, as sigsuspend and ppoll do, and keeps the old one for signal_restore_mask
/// @pre The caller is the thread
/// @note Does nothing while a replaced mask is still kept
void signal_replace_mask(thread_t* restrict thread, sigset_t mask);

/// Puts back the mask that signal_replace_mask replaced
/// @pre The caller is the thread
/// @note A handler that runs puts the mask back through sigreturn
void signal_restore_mask(thread_t* restrict thread);
sigset_t signal_pending(thread_t* restrict thread);

/// Takes a pending signal of the set, blocked or not, so that it runs no handler
/// @pre The caller is the thread
/// @param info Receives the siginfo of the signal
/// @return The number of the signal, or zero when none of the set is pending
int signal_take(thread_t* restrict thread, sigset_t set, siginfo_t* restrict info);

/// Whether the thread blocks the signal or its process ignores it
bool signal_refused(thread_t* restrict thread, int sig) __nodiscard;

/// Whether a signal that the thread could take now has a handler, so that it would return to user mode through the handler
bool signal_handler_pending(thread_t* restrict thread) __nodiscard;
void signal_flush(pending_signal_t* restrict queue);
bool signal_on_altstack(const thread_t* restrict thread, uintptr_t sp) __nodiscard;
void signal_altstack_get(const thread_t* restrict thread, uintptr_t sp, stack_t* restrict out);

/// Builds a signal frame on the user stack, or on the alternate stack with SA_ONSTACK, and points the context at the handler
/// @note The frame keeps the old context and old_mask for arch_signal_return. If this fails, the caller kills the process
/// @retval 0 The context now enters the handler
/// @retval -EFAULT The handler, the restorer or the stack is unusable
int arch_signal_setup(thread_t* restrict thread, hart_context_t* restrict ctx, const sigaction_t* restrict action,
    const siginfo_t* restrict info, sigset_t old_mask);

/// Restores the context from the frame at the user stack pointer, keeping
/// the flags userspace may not set, and hands back the saved signal mask
/// @param mask Receives the mask for the caller to apply
/// @retval 0 The context is restored
/// @retval -EFAULT The frame is unreadable or invalid
/// @retval -ENOMEM No memory to check the saved FPU state in
int arch_signal_return(thread_t* restrict thread, hart_context_t* restrict ctx, sigset_t* restrict mask);

/// Turns a system call rewound to run again into one that returns -EINTR,
/// for a handler without SA_RESTART interrupting it
void arch_syscall_interrupted(hart_context_t* restrict ctx);

static __always_inline __nodiscard sigset_t sigbit(int sig)
{
    return (sigset_t)(1) << (sig - 1);
}
