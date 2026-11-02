# System calls

A system call is how a process asks the kernel to do work for it. The number
in a register selects a handler from one table. The handler reads the
arguments and returns one value.

The mechanism has two halves. The generic half is the number blocks, the
table, the dispatcher and the handler contract. The arch half is the entry
stub, the register layout, the restart and the return to user. amd64 is the
only implemented arch, so the `syscall` instruction and every register name on
this page are amd64. The paragraphs that describe an instruction or a register
say so. A new port supplies its own arch half and keeps the generic contract.

## The path

```
 user space     | arch (amd64 today)  | generic kernel
 ---------------+---------------------+---------------------
                |                     |
 mov number/args|                     |
 syscall ------>| entry stub          |
                |  save the frame     |
                |  arch handler ----->| syscall_dispatch
                |                     |  table[number]
                |                     |  handler
                |  intptr_t      <----|
                |  restart and check  |
                |  exit_to_user ----->| deliver signals
                |  restore the frame  |
 sysretq <------| return stub         |
```

The middle column is the arch code. A port replaces it and keeps the right
column and the table. The names in the middle column are roles. The amd64
functions are `amd64_syscall_entry`, `amd64_syscall_handler` and the return
path in the same stub.

## The arch half, on amd64

Vex uses the SysV AMD64 ABI for a system call, as Linux does. `rax` holds the
number and the result. The arguments are in `rdi`, `rsi`, `rdx`, `r10`, `r8`
and `r9`. The fourth argument is `r10` and not `rcx`, because `syscall` keeps
the return address in `rcx`. The
[SysV ABI specification](https://gitlab.com/x86-psABIs/x86-64-ABI) defines
the rest of the register state.

The entry stub runs with the user stack still loaded. It swaps `gs` to the
kernel base, saves the user stack pointer in `thread::entry_scratch`, and
loads the kernel stack from the thread. It then pushes a `hart_context_t`
frame with the user registers. `usage_enter_kernel()` runs before `sti`, so
an interrupt in that window counts as system time. Interrupts are enabled
when the dispatcher runs.

`arch_init_syscall()` sets `EFER.SCE`, `STAR` and `LSTAR` on every hart, and
clears `TF`, `IF`, `DF` and `AC` in `SFMASK`. A call therefore starts with
interrupts disabled and with a clear direction flag.

The generic code never names a register of the hart. It reads the frame
through the `syscall_number`, `syscall_retval`, `syscall_arg_1` to
`syscall_arg_6` and `syscall_sp` macros of `<arch/hart.h>`. A port defines
these macros for its own registers, and its entry stub fills the frame. The
port also supplies the restart rewind and the return path in the next
section.

## Dispatch

`syscall_dispatch()` reads the number through `syscall_number`. A number at or
past `syscall_table_size` is not a call, and a null entry is not a call
either. Both return `-ENOSYS`. The function copies the six arguments into an
array, records the frame in `thread::syscall_ctx` and calls the handler.

`thread::syscall_ctx` stays valid for the whole call. `fork`, `sigreturn` and
the job control code read it.

A handler returns `intptr_t`. A negative value is an error, and the absolute
value is the errno number. Zero and positive values are results. The
dispatcher stores the value through `syscall_retval` and returns.

## The table

A handler declares itself with `define_syscall(function, number, count)`.
`count` is the number of registers that the call reads. The macro makes a
`syscall_t` entry in the `.syscalls` section, and the linker script brackets
that section with `unsorted_syscalls_start` and `unsorted_syscalls_end`.

`init_syscall()` builds the table once, at the syscall stage of the BSP
initcalls. It sizes the table from the highest number, allocates it with
`kmalloc`, and copies every entry into its slot. A duplicate number trips a
`kassert`. A slot that no call claimed stays null. The table does not change
after this point, and nothing removes a call.

Two other macros do work around the handler:

- `define_syscall_fd` resolves the first register as a descriptor and passes
  a `file_t*` in its place. The dispatcher holds a reference for the call and
  closes the file when the handler returns.
- `define_syscall_dirfd` copies the second register, a path, into a `PATH_MAX`
  buffer on the kernel stack. A path without a terminator fails with
  `-ENAMETOOLONG`. The handler gets the raw `dirfd` and the copied path, and
  resolves the base itself with `syscall_dirfd()` or `syscall_resolve_at()`.

A new macro is for work that the dispatcher must do around every call of that
shape. It is not for work that one handler does with its arguments.

## Number blocks

Each subsystem owns a block of 32 numbers, and the block starts at a multiple
of `0x20`. A new call takes the next free number in its block. Number 0 stays
invalid, so that a zeroed register fails with `ENOSYS` and not with a real
call. A gap in a block also fails with `ENOSYS`.

## Restart and signals

A handler that cannot finish yet returns `-ERESTART`. The arch handler keeps
the number, rewinds the saved program counter to the entry instruction, and
sets `thread::syscall_restart`. The call then runs again, unless a signal
needs a handler first.

`exit_to_user()` delivers the pending signals. A caught signal without
`SA_RESTART` replaces the restarted call with `-EINTR` and moves the program
counter past the entry instruction. A signal with `SA_RESTART` leaves the
rewind in place, and the call runs again. `SYS_SIGRETURN` is exempt, as its
frame brings back the interrupted result register.

On amd64, the rewind and the skip are two bytes, the size of the `syscall`
instruction. `arch_syscall_interrupted()` sets `-EINTR` in `rax` and moves
`RIP` and `RCX` by the same amount. Before it leaves, the handler checks that
the user `RIP` is below the end of the address space. A higher value kills the
process with `SIGSEGV`, because `SYSRET` with such a value faults in ring 0 on
Intel. If the frame no longer matches `RCX` and `R11`, the return goes through
`IRETQ` instead of `SYSRET`.

## Rules for a handler

- The signature of the handler is the signature of the call. The arguments
  are register-wide integers, except the object that a `_fd` or `_dirfd`
  macro passes in front.
- A call reads at most six registers. A seventh argument needs a struct or a
  pointer.
- A user pointer arrives as a `user_addr_t`. The handler copies through it
  with the `user_*` helpers, and never follows it as a pointer.
- A handler that the `_fd` macro wrapped must not close the file. The
  dispatcher owns that reference.
- The handler runs with interrupts enabled, and it may block.
- A negative return is an errno. Only `-ERESTART` is special, and only the
  arch return path sees it.

## See also

- [Syscalls](../reference/syscalls.md) lists every call, with its number,
  arguments and handler.

## Source

Generic:

- [syscall.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/syscall.c#L35) — dispatch, descriptors and path resolution
- [syscall_internal.h](https://github.com/vex-os/kernel/blob/main/kernel/proc/syscall_internal.h#L42) — the registration macros
- [syscall.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/syscall.h#L141) — the table entry and the dispatch interface
- [syscall.h (usr)](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/syscall.h#L4) — the numbers
- [signal.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/signal.c#L462) — `SA_RESTART` and `-EINTR`
- [user_exit.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/user_exit.c#L8) — pending work on the way out

amd64:

- [syscall.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/syscall.c#L24) — the handler, the restart and the MSRs
- [syscall.S](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/syscall.S#L5) — the entry and exit stubs
- [hart.h](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/include/arch/hart.h#L10) — the register aliases of `hart_context_t`
- [kernel.ld](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/kernel.ld#L45) — the `.syscalls` section
