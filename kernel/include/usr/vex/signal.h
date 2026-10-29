#ifndef USR_VEX_SIGNAL_H
#define USR_VEX_SIGNAL_H

#include <arch/signal.h>
#include <vex/types.h>

#define __k_SIGHUP    1
#define __k_SIGINT    2
#define __k_SIGQUIT   3
#define __k_SIGILL    4
#define __k_SIGTRAP   5
#define __k_SIGABRT   6
#define __k_SIGBUS    7
#define __k_SIGFPE    8
#define __k_SIGKILL   9
#define __k_SIGSEGV   10
#define __k_SIGPIPE   11
#define __k_SIGUSR1   12
#define __k_SIGUSR2   13
#define __k_SIGALRM   14
#define __k_SIGTERM   15
#define __k_SIGCHLD   16
#define __k_SIGCONT   17
#define __k_SIGSTOP   18
#define __k_SIGTSTP   19
#define __k_SIGTTIN   20
#define __k_SIGTTOU   21
#define __k_SIGURG    22
#define __k_SIGXCPU   23
#define __k_SIGXFSZ   24
#define __k_SIGVTALRM 25
#define __k_SIGPROF   26
#define __k_SIGWINCH  27
#define __k_SIGSYS    28
#define __k_SIGINFO   29
#define __k_SIGIO     30
#define __k_SIGPWR    31
#define __k_SIGRTMIN  32
#define __k_SIGRTMAX  64
#define __k_NSIG      64

typedef __k_uint64_t __k_sigset_t;

typedef void (*__k_sighandler_t)(__k_int32_t);

#define __k_SIG_DFL ((__k_sighandler_t)(0))
#define __k_SIG_IGN ((__k_sighandler_t)(1))

#define __k_SA_NOCLDSTOP 0x0001
#define __k_SA_NOCLDWAIT 0x0002
#define __k_SA_SIGINFO   0x0004
#define __k_SA_ONSTACK   0x0008
#define __k_SA_RESTART   0x0010
#define __k_SA_NODEFER   0x0020
#define __k_SA_RESETHAND 0x0040
#define __k_SA_RESTORER  0x0080

#define __k_SIG_BLOCK   0
#define __k_SIG_UNBLOCK 1
#define __k_SIG_SETMASK 2

/* Codes of any signal are negative, the positive ones belong to a signal */

#define __k_SI_USER    (-1)
#define __k_SI_QUEUE   (-2)
#define __k_SI_TIMER   (-3)
#define __k_SI_MESGQ   (-4)
#define __k_SI_ASYNCIO (-5)
#define __k_SI_KERNEL  (-6)
#define __k_SI_TKILL   (-7)

#define __k_ILL_ILLOPC 1
#define __k_ILL_ILLOPN 2
#define __k_ILL_ILLADR 3
#define __k_ILL_ILLTRP 4
#define __k_ILL_PRVOPC 5
#define __k_ILL_PRVREG 6
#define __k_ILL_COPROC 7
#define __k_ILL_BADSTK 8

#define __k_FPE_INTDIV 1
#define __k_FPE_INTOVF 2
#define __k_FPE_FLTDIV 3
#define __k_FPE_FLTOVF 4
#define __k_FPE_FLTUND 5
#define __k_FPE_FLTRES 6
#define __k_FPE_FLTINV 7
#define __k_FPE_FLTSUB 8

#define __k_SEGV_MAPERR 1
#define __k_SEGV_ACCERR 2

#define __k_BUS_ADRALN 1
#define __k_BUS_ADRERR 2
#define __k_BUS_OBJERR 3

#define __k_TRAP_BRKPT 1
#define __k_TRAP_TRACE 2

#define __k_CLD_EXITED    1
#define __k_CLD_KILLED    2
#define __k_CLD_DUMPED    3
#define __k_CLD_TRAPPED   4
#define __k_CLD_STOPPED   5
#define __k_CLD_CONTINUED 6

union __k_sigval {
    __k_int32_t sival_int;
    void* sival_ptr;
};

struct __k_siginfo {
    __k_int32_t si_signo;
    __k_int32_t si_code;
    __k_int32_t si_errno;
    __k_int32_t si_status;
    __k_pid_t si_pid;
    __k_uid_t si_uid;
    void* si_addr;
    union __k_sigval si_value;
};

struct __k_sigaction {
    __k_sighandler_t sa_handler;
    __k_uint64_t sa_flags;
    void (*sa_restorer)(void);
    __k_sigset_t sa_mask;
};

struct __k_stack {
    void* ss_sp;
    __k_size_t ss_size;
    __k_int32_t ss_flags;
    __k_uint32_t ss_reserved;
};

#define __k_SS_ONSTACK 0x0001
#define __k_SS_DISABLE 0x0002

#define __k_UC_FPU 0x0001 /* uc_mcontext points at the FPU state saved with it */

struct __k_ucontext {
    __k_uint64_t uc_flags;
    struct __k_ucontext* uc_link;
    struct __k_stack uc_stack;
    __k_sigset_t uc_sigmask;
    struct __k_mcontext uc_mcontext;
};

#endif /* USR_VEX_SIGNAL_H */
