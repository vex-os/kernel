#include <arch/amd64.fpu.h>
#include <arch/amd64.idt.h>
#include <vex/compiler.h>
#include <vex/cputime.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/kstack.h>
#include <vex/linker.h>
#include <vex/panic.h>
#include <vex/pmap.h>
#include <vex/process.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/vms.h>

extern void amd64_isr_00(void);
extern void amd64_isr_01(void);
extern void amd64_isr_02(void);
extern void amd64_isr_03(void);
extern void amd64_isr_04(void);
extern void amd64_isr_05(void);
extern void amd64_isr_06(void);
extern void amd64_isr_07(void);
extern void amd64_isr_08(void);
extern void amd64_isr_09(void);
extern void amd64_isr_0A(void);
extern void amd64_isr_0B(void);
extern void amd64_isr_0C(void);
extern void amd64_isr_0D(void);
extern void amd64_isr_0E(void);
extern void amd64_isr_0F(void);
extern void amd64_isr_10(void);
extern void amd64_isr_11(void);
extern void amd64_isr_12(void);
extern void amd64_isr_13(void);
extern void amd64_isr_14(void);
extern void amd64_isr_15(void);
extern void amd64_isr_16(void);
extern void amd64_isr_17(void);
extern void amd64_isr_18(void);
extern void amd64_isr_19(void);
extern void amd64_isr_1A(void);
extern void amd64_isr_1B(void);
extern void amd64_isr_1C(void);
extern void amd64_isr_1D(void);
extern void amd64_isr_1E(void);
extern void amd64_isr_1F(void);

static void page_fault_handler(hart_local_t* restrict hart, hart_context_t* restrict ctx)
{
    uintptr_t fault;
    asm volatile("movq %%cr2, %0" : "=r"(fault));

    unsigned access;

    if(ctx->amd64_err & 0x2) {
        access = VPROT_WRITE;
    }
    else if(ctx->amd64_err & 0x10) {
        access = VPROT_EXEC;
    }
    else {
        access = VPROT_READ;
    }

    if(3 != (ctx->amd64_cs & 3)) {
        panic("kernel page fault: hart=%hu fault=%p rip=%p access=%02X present=%d", hart->index, (void*)(fault), (void*)(ctx->amd64_rip),
            access, (int)(ctx->amd64_err & 0x1));
    }

    thread_t* thread = hart->curr_thread;
    process_t* process = thread->process;

    if(process == nullptr) {
        sched_terminate();
        unreachable();
    }

    // A fault from ring 3 holds
    // no kernel locks, so resolving
    // or signalling may sleep
    enable_interrupts();

    int retval = vms_fault(process->vmspace, virt_addr(fault), access);

    if(retval < 0) {
        siginfo_t info = {};
        info.si_signo = SIGSEGV;
        info.si_code = SEGV_MAPERR;
        info.si_addr = (void*)(fault);

        if(retval == -EACCES) {
            info.si_code = SEGV_ACCERR;
        }
        else if(retval != -EFAULT) {
            info.si_signo = SIGBUS;
            info.si_code = BUS_ADRERR;
        }

        signal_force(thread, &info);
    }

    // The way back to ring 3 swaps GS
    // and an interrupt landing after
    // that would take the user GS for the kernel GS
    disable_interrupts();
}

static int fpu_exception_code(bool simd)
{
    uint32_t raised = fpu_unmasked_exceptions(simd);

    if(raised & FPU_EXCEPTION_IE) {
        return FPE_FLTINV;
    }

    if(raised & FPU_EXCEPTION_ZE) {
        return FPE_FLTDIV;
    }

    if(raised & FPU_EXCEPTION_OE) {
        return FPE_FLTOVF;
    }

    if(raised & (FPU_EXCEPTION_DE | FPU_EXCEPTION_UE)) {
        return FPE_FLTUND;
    }

    if(raised & FPU_EXCEPTION_PE) {
        return FPE_FLTRES;
    }

    return SI_KERNEL;
}

static void user_exception_info(uint64_t intvec, const hart_context_t* restrict ctx, siginfo_t* restrict info)
{
    info->si_signo = SIGSEGV;
    info->si_code = SI_KERNEL;
    info->si_addr = (void*)(ctx->amd64_rip);

    switch(intvec) {
        case 0x00:
            info->si_signo = SIGFPE;
            info->si_code = FPE_INTDIV;
            break;
        case 0x01:
            info->si_signo = SIGTRAP;
            info->si_code = TRAP_TRACE;
            break;
        case 0x03:
            info->si_signo = SIGTRAP;
            info->si_code = TRAP_BRKPT;
            break;
        case 0x06:
            info->si_signo = SIGILL;
            info->si_code = ILL_ILLOPC;
            break;
        case 0x10:
            info->si_signo = SIGFPE;
            info->si_code = fpu_exception_code(false);
            break;
        case 0x13:
            info->si_signo = SIGFPE;
            info->si_code = fpu_exception_code(true);
            break;
        case 0x11:
            info->si_signo = SIGBUS;
            info->si_code = BUS_ADRALN;
            break;
        default:
            info->si_addr = nullptr;
            break;
    }
}

static bool iretq_to_user_faulted(const hart_context_t* restrict ctx, uint64_t intvec)
{
    if(intvec != 0x0B && intvec != 0x0C && intvec != 0x0D) {
        return false;
    }

    if(3 == (ctx->amd64_cs & 3)) {
        return false;
    }

    const char* insn = (const char*)(ctx->amd64_rip);

    if(insn < text_start || insn + 2 > text_end) {
        return false;
    }

    if((uint8_t)(insn[0]) != 0x48 || (uint8_t)(insn[1]) != 0xCF) {
        return false;
    }

    const uint64_t* frame = (const uint64_t*)(ctx->amd64_rsp);

    return 3 == (frame[1] & 3);
}

void __used amd64_isr_handler(hart_context_t* restrict ctx, uint64_t intvec)
{
    disable_interrupts();

    if(intvec == 0x02) {
        panic_nmi();
    }

    hart_local_t* hart = this_hart();

    // Harts only get set up at the multicore stage
    // so anything happening before that quite literally
    // means it's joever and we're all gonna die
    if(hart == nullptr) {
        uintptr_t fault;
        asm volatile("movq %%cr2, %0" : "=r"(fault));
        panic("early exception 0x%02zX: rip=%p cr2=%p err=0x%zX", (size_t)(intvec), (void*)(ctx->amd64_rip), (void*)(fault),
            (size_t)(ctx->amd64_err));
    }

    if(3 == (ctx->amd64_cs & 3)) {
        cputime_enter_kernel();
    }

    if(intvec == 0x0E) {
        page_fault_handler(hart, ctx);
        return;
    }

    if(intvec == 0x08) {
        uintptr_t cr2;
        asm volatile("movq %%cr2, %0" : "=r"(cr2));

        virt_addr_t fault = virt_addr(cr2);

        if(kstack_is_guard(fault)) {
            panic("kernel stack overflow: hart=%hu fault=%p rip=%p", hart->index, virt_addr_pointer(fault), (void*)(ctx->amd64_rip));
        }

        panic("double fault: hart=%hu rip=%p", hart->index, (void*)(ctx->amd64_rip));
    }

    if(3 == (ctx->amd64_cs & 3) || iretq_to_user_faulted(ctx, intvec)) {
        thread_t* thread = hart->curr_thread;
        process_t* process = thread->process;

        if(process == nullptr) {
            sched_terminate();
            unreachable();
        }

        enable_interrupts();

        // The frame IRETQ choked on is the
        // one it would retry, so there is no going back
        if(3 != (ctx->amd64_cs & 3)) {
            process_kill(process, SIGSEGV);
            sched_terminate();
            unreachable();
        }

        siginfo_t info = {};
        user_exception_info(intvec, ctx, &info);
        signal_force(thread, &info);

        disable_interrupts();
        return;
    }

    panic("idt: unhandled exception 0x%02zX", (size_t)(intvec));
}

static void arch_init_isr(void)
{
    idt_set_kernel_gate(0x00, false, &amd64_isr_00);
    idt_set_kernel_gate(0x01, false, &amd64_isr_01);
    idt_set_ist_gate(0x02, IDT_IST_NMI, &amd64_isr_02);
    idt_set_user_gate(0x03, false, &amd64_isr_03);
    idt_set_user_gate(0x04, false, &amd64_isr_04);
    idt_set_kernel_gate(0x05, false, &amd64_isr_05);
    idt_set_kernel_gate(0x06, false, &amd64_isr_06);
    idt_set_kernel_gate(0x07, false, &amd64_isr_07);
    idt_set_ist_gate(0x08, IDT_IST_DOUBLE_FAULT, &amd64_isr_08);
    idt_set_kernel_gate(0x09, false, &amd64_isr_09);
    idt_set_kernel_gate(0x0A, false, &amd64_isr_0A);
    idt_set_kernel_gate(0x0B, false, &amd64_isr_0B);
    idt_set_kernel_gate(0x0C, false, &amd64_isr_0C);
    idt_set_kernel_gate(0x0D, false, &amd64_isr_0D);
    idt_set_kernel_gate(0x0E, false, &amd64_isr_0E);
    idt_set_kernel_gate(0x0F, false, &amd64_isr_0F);
    idt_set_kernel_gate(0x10, false, &amd64_isr_10);
    idt_set_kernel_gate(0x11, false, &amd64_isr_11);
    idt_set_ist_gate(0x12, IDT_IST_MACHINE_CHECK, &amd64_isr_12);
    idt_set_kernel_gate(0x13, false, &amd64_isr_13);
    idt_set_kernel_gate(0x14, false, &amd64_isr_14);
    idt_set_kernel_gate(0x15, false, &amd64_isr_15);
    idt_set_kernel_gate(0x16, false, &amd64_isr_16);
    idt_set_kernel_gate(0x17, false, &amd64_isr_17);
    idt_set_kernel_gate(0x18, false, &amd64_isr_18);
    idt_set_kernel_gate(0x19, false, &amd64_isr_19);
    idt_set_kernel_gate(0x1A, false, &amd64_isr_1A);
    idt_set_kernel_gate(0x1B, false, &amd64_isr_1B);
    idt_set_kernel_gate(0x1C, false, &amd64_isr_1C);
    idt_set_kernel_gate(0x1D, false, &amd64_isr_1D);
    idt_set_kernel_gate(0x1E, false, &amd64_isr_1E);
    idt_set_kernel_gate(0x1F, false, &amd64_isr_1F);
}

initcall_bsp_arch(arch_init_isr, INIT_MIDDLE);
