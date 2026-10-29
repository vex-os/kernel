#include <arch/amd64.fpu.h>
#include <arch/hart.h>
#include <vex/errno.h>
#include <vex/pmap.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/signal.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vms.h>

#define RED_ZONE 128

// The flags a handler may hand back
// through sigreturn: arithmetic, TF, DF, AC and RF
#define AMD64_RFLAGS_USER UINT64_C(0x0000000000050DD5)

typedef struct sigframe_s sigframe_t;

// The handler's ret pops restorer which
// leaves the stack pointer at uc for sigreturn
struct sigframe_s {
    uint64_t restorer;
    ucontext_t uc;
    siginfo_t info;
};

static void save_mcontext(mcontext_t* restrict mc, const hart_context_t* restrict ctx)
{
    mc->mc_rax = ctx->amd64_rax;
    mc->mc_rbx = ctx->amd64_rbx;
    mc->mc_rcx = ctx->amd64_rcx;
    mc->mc_rdx = ctx->amd64_rdx;
    mc->mc_rsi = ctx->amd64_rsi;
    mc->mc_rdi = ctx->amd64_rdi;
    mc->mc_rbp = ctx->amd64_rbp;
    mc->mc_rsp = ctx->amd64_rsp;
    mc->mc_r8 = ctx->amd64_r8;
    mc->mc_r9 = ctx->amd64_r9;
    mc->mc_r10 = ctx->amd64_r10;
    mc->mc_r11 = ctx->amd64_r11;
    mc->mc_r12 = ctx->amd64_r12;
    mc->mc_r13 = ctx->amd64_r13;
    mc->mc_r14 = ctx->amd64_r14;
    mc->mc_r15 = ctx->amd64_r15;
    mc->mc_rip = ctx->amd64_rip;
    mc->mc_rflags = ctx->amd64_rflags;
}

static void load_mcontext(hart_context_t* restrict ctx, const mcontext_t* restrict mc)
{
    ctx->amd64_rax = mc->mc_rax;
    ctx->amd64_rbx = mc->mc_rbx;
    ctx->amd64_rcx = mc->mc_rcx;
    ctx->amd64_rdx = mc->mc_rdx;
    ctx->amd64_rsi = mc->mc_rsi;
    ctx->amd64_rdi = mc->mc_rdi;
    ctx->amd64_rbp = mc->mc_rbp;
    ctx->amd64_rsp = mc->mc_rsp;
    ctx->amd64_r8 = mc->mc_r8;
    ctx->amd64_r9 = mc->mc_r9;
    ctx->amd64_r10 = mc->mc_r10;
    ctx->amd64_r11 = mc->mc_r11;
    ctx->amd64_r12 = mc->mc_r12;
    ctx->amd64_r13 = mc->mc_r13;
    ctx->amd64_r14 = mc->mc_r14;
    ctx->amd64_r15 = mc->mc_r15;
    ctx->amd64_rip = mc->mc_rip;
    ctx->amd64_rflags = (ctx->amd64_rflags & ~AMD64_RFLAGS_USER) | (mc->mc_rflags & AMD64_RFLAGS_USER);
}

int arch_signal_setup(thread_t* restrict thread, hart_context_t* restrict ctx, const sigaction_t* restrict action,
    const siginfo_t* restrict info, sigset_t old_mask)
{
    process_t* process = thread->process;
    virt_addr_t user_end = pmap_user_end(process->vmspace->pmap);

    uintptr_t handler = (uintptr_t)(action->sa_handler);
    uintptr_t restorer = (uintptr_t)(action->sa_restorer);

    if(address_greater_equal(virt_addr(handler), user_end) || address_greater_equal(virt_addr(restorer), user_end)) {
        return -EFAULT;
    }

    bool on_altstack = signal_on_altstack(thread, ctx->amd64_rsp);

    // The red zone belongs to the interrupted
    // code and an alternate stack has none to keep
    uintptr_t top = ctx->amd64_rsp - RED_ZONE;
    uintptr_t bottom = 0;

    if((action->sa_flags & SA_ONSTACK) && thread->sig_altstack_size && !on_altstack) {
        top = thread->sig_altstack + thread->sig_altstack_size;
        bottom = thread->sig_altstack;
    }

    uintptr_t fpu_user = align_floor(top - fpu_state_size, FPU_STATE_ALIGN);
    uintptr_t sp = align_floor(fpu_user - sizeof(sigframe_t), 16) - sizeof(uint64_t); // As if call pushed restorer

    if(top > user_end.value || ctx->amd64_rsp > user_end.value || sp >= top || sp < bottom) {
        return -EFAULT;
    }

    void* fpu_state = fpu_state_of(thread);
    fpu_save(fpu_state);

    if(user_copyout(fpu_user, fpu_state, fpu_state_size) < 0) {
        return -EFAULT;
    }

    sigframe_t frame = {};
    frame.restorer = restorer;
    frame.uc.uc_flags = UC_FPU;
    signal_altstack_get(thread, ctx->amd64_rsp, &frame.uc.uc_stack);

    frame.uc.uc_sigmask = old_mask;
    frame.uc.uc_mcontext.mc_fpu_state = (void*)(fpu_user);
    frame.uc.uc_mcontext.mc_fpu_size = fpu_state_size;
    frame.info = *info;

    save_mcontext(&frame.uc.uc_mcontext, ctx);

    if(user_copyout(sp, &frame, sizeof(frame)) < 0) {
        return -EFAULT;
    }

    ctx->amd64_rip = handler;
    ctx->amd64_rsp = sp;
    ctx->amd64_rdi = info->si_signo;
    ctx->amd64_rsi = sp + offsetof(sigframe_t, info);
    ctx->amd64_rdx = sp + offsetof(sigframe_t, uc);
    ctx->amd64_rax = 0;
    ctx->amd64_rflags &= ~(AMD64_RFLAGS_TF | AMD64_RFLAGS_DF);

    // SYSRETQ takes these two, and the frame holds their old values
    ctx->amd64_rcx = ctx->amd64_rip;
    ctx->amd64_r11 = ctx->amd64_rflags;

    fpu_reset();

    return 0;
}

int arch_signal_return(thread_t* restrict thread, hart_context_t* restrict ctx, sigset_t* restrict mask)
{
    process_t* process = thread->process;
    virt_addr_t user_end = pmap_user_end(process->vmspace->pmap);

    ucontext_t uc;

    if(user_copyin(&uc, ctx->amd64_rsp, sizeof(uc)) < 0) {
        return -EFAULT;
    }

    mcontext_t* mc = &uc.uc_mcontext;

    if(address_greater_equal(virt_addr(mc->mc_rip), user_end)) {
        return -EFAULT;
    }

    if(uc.uc_flags & UC_FPU) {
        // A switch away mid-copy would save over the thread's own area, so the image waits elsewhere
        void* image = pslab_alloc_virt();

        if(image == nullptr) {
            return -ENOMEM;
        }

        bool valid = user_copyin(image, (uintptr_t)(mc->mc_fpu_state), fpu_state_size) >= 0 && fpu_state_valid(image);

        if(valid) {
            fpu_load(image);
        }

        pslab_free_virt(image);

        if(!valid) {
            return -EFAULT;
        }
    }

    load_mcontext(ctx, mc);
    *mask = uc.uc_sigmask;

    return 0;
}
