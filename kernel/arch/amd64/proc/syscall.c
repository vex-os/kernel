#include <arch/amd64.gdt.h>
#include <arch/amd64.msr.h>
#include <arch/hart.h>
#include <vex/errno.h>
#include <vex/initcall.h>
#include <vex/pmap.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/types.h>
#include <vex/vms.h>

extern void amd64_syscall_entry(void);

#define SYSRET_BASE       gdt_selector(GDT_KERN_DATA_64, 0, 3)
#define SYSCALL_INSN_SIZE 2

// SYSRET to a non-canonical RCX faults in ring 0 on Intel, but on the
// user's stack already; no return address past the user half gets there.
// RIP is the one checked: SYSRET only runs when RCX matches it, and after
// sigreturn RCX is whatever the interrupted code kept in that register
void __used amd64_syscall_handler(hart_context_t* restrict ctx)
{
    uint64_t number = ctx->syscall_number;

    syscall_dispatch(ctx);

    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process && address_greater_equal(virt_addr(ctx->amd64_rip), pmap_user_end(process->vmspace->pmap))) {
        process_kill(process, SIGSEGV);
        sched_terminate();
        unreachable();
    }

    // Sigreturn hands back the interrupted RAX, which may hold -ERESTART by chance
    if(number != SYS_SIGRETURN && (intptr_t)(ctx->syscall_retval) == -ERESTART) {
        ctx->syscall_number = number;
        ctx->amd64_rip -= SYSCALL_INSN_SIZE;
        ctx->amd64_rcx -= SYSCALL_INSN_SIZE;
        thread->syscall_restart = true;
    }

    disable_interrupts();
    exit_to_user(ctx);

    thread->syscall_restart = false;
}

void arch_syscall_interrupted(hart_context_t* restrict ctx)
{
    ctx->syscall_retval = -EINTR;
    ctx->amd64_rip += SYSCALL_INSN_SIZE;
    ctx->amd64_rcx += SYSCALL_INSN_SIZE;
}

static void arch_init_syscall(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | MSR_EFER_SCE);

    // SYSRET takes SS from STAR[63:48] + 8 and CS from STAR[63:48] + 16;
    // Intel forces both RPLs to 3, but AMD leaves SS with the base's RPL,
    // and a user SS with RPL 0 faults the next IRETQ back to ring 3
    static_assert(SYSRET_BASE + 8 == gdt_selector(GDT_USER_DATA_64, 0, 3));
    static_assert(SYSRET_BASE + 16 == gdt_selector(GDT_USER_CODE_64, 0, 3));

    uint64_t star_value = UINT64_C(0);
    star_value |= ((uint64_t)(SYSRET_BASE) << 48);
    star_value |= ((uint64_t)(gdt_selector(GDT_KERN_CODE_64, 0, 0)) << 32); // STAR[47:32] = kernel code segment selector
    wrmsr(MSR_STAR, star_value);

    wrmsr(MSR_LSTAR, (uintptr_t)(&amd64_syscall_entry));
    wrmsr(MSR_SFMASK, AMD64_RFLAGS_TF | AMD64_RFLAGS_IF | AMD64_RFLAGS_DF | AMD64_RFLAGS_AC);
}

initcall_smp_syscall(arch_init_syscall, INIT_EARLY);
