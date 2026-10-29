#include <arch/amd64.fpu.h>
#include <arch/amd64.gdt.h>
#include <arch/amd64.msr.h>
#include <vex/hart.h>
#include <vex/pmap.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/vms.h>

void* fpu_state_of(const thread_t* restrict thread)
{
    if(thread->process == nullptr) {
        return nullptr;
    }

    uintptr_t ctx = (uintptr_t)(thread_context(thread));

    return (void*)(align_floor(ctx - fpu_state_size, FPU_STATE_ALIGN));
}

void arch_thread_setup_kern(thread_t* restrict thread, void* restrict entry, void* restrict arg)
{
    hart_context_t* ctx = thread_context(thread);

    memset(ctx, 0, sizeof(hart_context_t));

    ctx->amd64_ds = gdt_selector(GDT_KERN_DATA_64, 0, 0);
    ctx->amd64_es = gdt_selector(GDT_KERN_DATA_64, 0, 0);
    ctx->amd64_cs = gdt_selector(GDT_KERN_CODE_64, 0, 0);
    ctx->amd64_ss = gdt_selector(GDT_KERN_DATA_64, 0, 0);
    ctx->amd64_rip = (uintptr_t)(entry);
    ctx->amd64_rsp = (uintptr_t)(ctx);
    ctx->amd64_rdi = (uintptr_t)(arg);
    ctx->amd64_rflags = AMD64_RFLAGS_FIXED | AMD64_RFLAGS_IF;

    thread->arch.gs_base = thread;
    thread->arch.fs_base = nullptr;
    thread->kernel_rsp = ctx;

    // A kernel thread made from within a system call
    // must not keep that process's address space alive
    thread->arch.cr3 = sys_pmap.phys.value;
}

void arch_thread_setup_user(thread_t* restrict thread, void* restrict entry, void* restrict user_stack_top, uintptr_t arg, uintptr_t tcb,
    vmspace_t* restrict vmspace)
{
    hart_context_t* ctx = thread_context(thread);
    void* fpu_state = fpu_state_of(thread);

    fpu_init_state(fpu_state);

    memset(ctx, 0, sizeof(hart_context_t));

    ctx->amd64_ds = gdt_selector(GDT_USER_DATA_64, 0, 3);
    ctx->amd64_es = gdt_selector(GDT_USER_DATA_64, 0, 3);
    ctx->amd64_cs = gdt_selector(GDT_USER_CODE_64, 0, 3);
    ctx->amd64_ss = gdt_selector(GDT_USER_DATA_64, 0, 3);
    ctx->amd64_rip = (uintptr_t)(entry);
    ctx->amd64_rsp = (uintptr_t)(user_stack_top);
    ctx->amd64_rdi = arg;
    ctx->amd64_rflags = AMD64_RFLAGS_FIXED | AMD64_RFLAGS_IF;

    thread->arch.gs_base = nullptr;
    thread->arch.fs_base = (void*)(tcb);
    thread->arch.cr3 = vmspace->pmap->phys.value;
    thread->kernel_rsp = fpu_state;
}

void arch_thread_setup_fork(thread_t* restrict thread, const hart_context_t* restrict ctx, vmspace_t* restrict vmspace)
{
    hart_context_t* child_ctx = thread_context(thread);
    void* fpu_state = fpu_state_of(thread);

    fpu_save(fpu_state);

    memcpy(child_ctx, ctx, sizeof(hart_context_t));
    child_ctx->syscall_retval = 0;

    // Inside the kernel, userspace's GS base waits in KERNEL_GS_BASE
    thread->arch.gs_base = (void*)((uintptr_t)(rdmsr(MSR_KERNEL_GS_BASE)));
    thread->arch.fs_base = (void*)((uintptr_t)(rdmsr(MSR_FS_BASE)));
    thread->arch.cr3 = vmspace->pmap->phys.value;
    thread->kernel_rsp = fpu_state;
}

void arch_thread_exec(thread_t* restrict thread, vmspace_t* restrict vmspace, uintptr_t entry, uintptr_t user_stack_top)
{
    hart_context_t* ctx = thread->syscall_ctx;

    memset(ctx, 0, sizeof(hart_context_t));

    ctx->amd64_ds = gdt_selector(GDT_USER_DATA_64, 0, 3);
    ctx->amd64_es = gdt_selector(GDT_USER_DATA_64, 0, 3);
    ctx->amd64_cs = gdt_selector(GDT_USER_CODE_64, 0, 3);
    ctx->amd64_ss = gdt_selector(GDT_USER_DATA_64, 0, 3);
    ctx->amd64_rip = entry;
    ctx->amd64_rsp = user_stack_top;
    ctx->amd64_rflags = AMD64_RFLAGS_FIXED | AMD64_RFLAGS_IF;

    // sysretq takes these instead of
    // the frame's rip, rflags and rsp
    ctx->amd64_rcx = entry;
    ctx->amd64_r11 = AMD64_RFLAGS_FIXED | AMD64_RFLAGS_IF;
    thread->entry_scratch = (void*)(user_stack_top);

    thread->arch.gs_base = nullptr;
    thread->arch.fs_base = nullptr;

    // Switching away in between would
    // save the old CR3 back into the thread
    bool intr_state = disable_interrupts();

    wrmsr(MSR_KERNEL_GS_BASE, 0);
    wrmsr(MSR_FS_BASE, 0);
    fpu_reset();

    thread->arch.cr3 = vmspace->pmap->phys.value;
    pmap_switch(vmspace->pmap);

    restore_interrupts(intr_state);
}

void arch_thread_set_tcb(thread_t* restrict thread, uintptr_t tcb)
{
    bool intr_state = disable_interrupts();

    thread->arch.fs_base = (void*)(tcb);
    wrmsr(MSR_FS_BASE, tcb);

    restore_interrupts(intr_state);
}

void arch_thread_ctx_save(thread_t* restrict thread)
{
    thread->arch.gs_base = (void*)((uintptr_t)(rdmsr(MSR_KERNEL_GS_BASE)));
    thread->arch.fs_base = (void*)((uintptr_t)(rdmsr(MSR_FS_BASE)));

    uint64_t cr3;
    asm volatile("movq %%cr3, %0" : "=r"(cr3));
    thread->arch.cr3 = cr3;

    void* fpu_state = fpu_state_of(thread);

    if(fpu_state) {
        fpu_save(fpu_state);
    }
}

void arch_thread_ctx_load(thread_t* restrict thread)
{
    hart_local_t* hart = this_hart();

    hart->arch.tss.rsp_0 = (uintptr_t)(thread->kernel_rsp);

    wrmsr(MSR_GS_BASE, (uintptr_t)(thread));
    wrmsr(MSR_FS_BASE, (uintptr_t)(thread->arch.fs_base));
    wrmsr(MSR_KERNEL_GS_BASE, (uintptr_t)(thread->arch.gs_base));

    asm volatile("movq %0, %%cr3" ::"r"(thread->arch.cr3) : "memory");

    void* fpu_state = fpu_state_of(thread);

    if(fpu_state) {
        fpu_load(fpu_state);
    }
}
