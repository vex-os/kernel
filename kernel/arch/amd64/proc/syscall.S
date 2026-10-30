.section .text

.global amd64_syscall_entry
.type amd64_syscall_entry, @function
amd64_syscall_entry:
    swapgs                      # %gs:0                         -> thread::self

    movq %rsp, %gs:16           # thread::entry_scratch         <- user rsp for later sysretq
    movq %gs:8, %rsp            # thread::kernel_rsp

    pushq $0x3B                 # hart_context::amd64_ss        <- gdt_selector(GDT_USER_DATA_64, 0, 3)
    pushq %gs:16                # hart_context::amd64_rsp
    pushq %r11                  # hart_context::amd64_rflags
    pushq $0x43                 # hart_context::amd64_cs        <- gdt_selector(GDT_USER_CODE_64, 0, 3)
    pushq %rcx                  # hart_context::amd64_rip
    pushq $0x0000000000000000   # hart_context::amd64_err
    pushq %r15                  # hart_context::amd64_r15
    pushq %r14                  # hart_context::amd64_r14
    pushq %r13                  # hart_context::amd64_r13
    pushq %r12                  # hart_context::amd64_r12
    pushq %r11                  # hart_context::amd64_r11
    pushq %r10                  # hart_context::amd64_r10
    pushq %r9                   # hart_context::amd64_r9
    pushq %r8                   # hart_context::amd64_r8
    pushq %rbp                  # hart_context::amd64_rbp
    pushq %rdi                  # hart_context::amd64_rdi
    pushq %rsi                  # hart_context::amd64_rsi
    pushq %rdx                  # hart_context::amd64_rdx
    pushq %rcx                  # hart_context::amd64_rcx
    pushq %rbx                  # hart_context::amd64_rbx
    pushq %rax                  # hart_context::amd64_rax
    movq %es, %rax
    pushq %rax                  # hart_context::amd64_es
    movq %ds, %rax
    pushq %rax                  # hart_context::amd64_ds

    # Before STI, so that an interrupt in between counts as system time
    call usage_enter_kernel

    sti

    movq %rsp, %rdi
    call amd64_syscall_handler

    cli

    # SYSRETQ reloads rip and rflags from rcx and r11, so a frame they no
    # longer match, such as the one sigreturn restores, goes out through IRETQ
    movq 32(%rsp), %rax         # hart_context::amd64_rcx
    cmpq 144(%rsp), %rax        # hart_context::amd64_rip
    jne amd64_return_to_user
    movq 96(%rsp), %rax         # hart_context::amd64_r11
    cmpq 160(%rsp), %rax        # hart_context::amd64_rflags
    jne amd64_return_to_user
    movq 168(%rsp), %rax        # hart_context::amd64_rsp
    movq %rax, %gs:16           # thread::entry_scratch         <- a signal frame moves the stack

    popq %rax                   # hart_context::amd64_ds
    movq %rax, %ds
    popq %rax                   # hart_context::amd64_es
    movq %rax, %es
    popq %rax                   # hart_context::amd64_rax
    popq %rbx                   # hart_context::amd64_rbx
    popq %rcx                   # hart_context::amd64_rcx
    popq %rdx                   # hart_context::amd64_rdx
    popq %rsi                   # hart_context::amd64_rsi
    popq %rdi                   # hart_context::amd64_rdi
    popq %rbp                   # hart_context::amd64_rbp
    popq %r8                    # hart_context::amd64_r8
    popq %r9                    # hart_context::amd64_r9
    popq %r10                   # hart_context::amd64_r10
    popq %r11                   # hart_context::amd64_r11
    popq %r12                   # hart_context::amd64_r12
    popq %r13                   # hart_context::amd64_r13
    popq %r14                   # hart_context::amd64_r14
    popq %r15                   # hart_context::amd64_r15

    movq %gs:16, %rsp           # thread::entry_scratch         <- restore user rsp for sysretq

    swapgs                      # %gs:0                         -> whatever userspace had

    # SYSRET with a non-canonical RCX faults in ring 0 on Intel,
    # so whatever rewrites amd64_rip has to keep it in the user half,
    # as exec does by rejecting such entry points
    sysretq
