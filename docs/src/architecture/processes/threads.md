# Threads

Vex schedules threads, not processes. A `process_t` owns the address space,
the credentials, the descriptor table and the signal actions. A `thread_t`
owns the register context, the kernel stack and the state that a hart needs
to run it. The process ends when its last thread leaves.
[Scheduler](scheduler.md) covers where a thread runs.

## The two objects

`process_t` in `vex/process.h` holds `pid`, `ppid`, `flags` and `vmspace`.
It also holds the `threads` list, the child list, `credentials`, `rlimits`,
the descriptor table and `cwd`. `process.c` caps its size at
`PROCESS_SIZE_BUDGET` bytes with a `static_assert`, and it comes from the
slab.

Every process sits in one bucket of `proctable[PROCTABLE_BUCKETS]`, keyed by
`pid % PROCTABLE_BUCKETS` and chained through `hash_next`. `process_locate()`
takes `proctable_lock` with interrupts disabled, because the scheduler looks
up parents there.

`thread_t` in `vex/thread.h` holds `tid`, `state`, the scheduling locks and
`affinity`. It also holds `pending_work`, the signal masks and queue, `usage`,
`kstack_top`, `io_page` and `arch`. Its `process_next` and `process_prev`
links put it on the list of its process, under `process->lock`.

## Process lifetime

`process_create()` builds the object and returns it with one reference. It
creates the `vmspace` and takes the `rlimits` of the parent, or creates them.
It allocates the signal actions and two `SIGCHLD` notes, `exit_note` and
`adopt_note`. It takes the pid from `process_alloc_id()`. Then it adds the
process to the child list of the parent and to the process table.
`spawn_init()` calls it with `nullptr`.

A user thread comes from `thread_create_user()`, `thread_create_fork()`,
`thread_create_spawn()` or `thread_create_sibling()`. Each calls
`attach_thread_user()`, which adds the thread and takes a reference, so the
process outlives it.

The exit has two halves, and one thread does not do both:

```
begin_exit
  sets PROCESS_EXITING once, records exit_code and exit_signal
  posts THREAD_WORK_EXIT on every thread
      |
      v
each thread, on the way out
  process_finish_exit: itimer, terminal, groups, files
      |
      v
the last reap, in the scheduler
  vmspace released, PROCESS_ZOMBIE set, SIGCHLD to the parent
      |
      v
the waiter
  process_claim -> process_destroy
```

`begin_exit()` sets `PROCESS_EXITING` with one atomic OR, so only the first
call records `exit_code` and `exit_signal`. It posts `THREAD_WORK_EXIT` on
every thread under `process->lock` and calls `sched_kick()` for each one.

A thread acts on that bit only when it returns to user mode, in
`exit_to_user()`. A thread in an interruptible wait leaves that wait when the
wakeup arrives. Nothing stops a thread in the middle of a system call.

`process_thread_exit()` decrements `num_live_threads` for a thread that is not
already leaving. The thread that finds it at zero calls
`process_exit(process, 0)`. The process ends with its last live thread.

The half that may sleep runs in the exiting thread, not in the scheduler.
`process_finish_exit()` cancels the itimer and hangs up the terminal of a
session leader. It hangs up the groups that the exit orphans and calls
`release_files()`. `PROCESS_EXIT_FINISHED` makes the call idempotent, and a
filesystem close may block here.

`process_thread_reaped()` runs once for each thread that the scheduler frees.
For the last one it releases the `vmspace` with
`process_set_vmspace(process, nullptr)`, because no hart has those page tables
loaded anymore. It sets `PROCESS_ZOMBIE` and triggers `init_exited` for init.
Then it sends `SIGCHLD` to the parent and moves the children to init.

The event goes out before the signal, so a handler that calls `waitpid()`
finds the zombie. `process_claim()` then hands it to exactly one waiter.
`process_destroy()` unlinks it from the process table and the child list and
releases the files again. It also detaches the group and drops the reference
that `process_create()` returned.

## Thread lifetime

A new thread starts in `THREAD_BLOCKED`. The caller publishes it with
`sched_enqueue()`, and the `thread_spawn_*` forms and `sys_thread_create()`
do that themselves. A thread attached while the process is already exiting
gets `THREAD_WORK_EXIT` at once under the same lock. So does a thread attached
while another thread execs. Neither can slip past the walk in `begin_exit()`
or `process_dethread()`.

`attach_thread_user()` gives the first thread of a process the pid as its tid.
A later thread takes a new id from `process_alloc_id()`, the same counter.
That is why a tid never names a thread of another process.

`sys_thread_create()` receives the entry, the user stack top, the TCB, the
argument and the address of the clear word. It checks each address against
`pmap_user_end()` and then calls `thread_create_sibling()`.

`sys_thread_exit()` leaves the process first, with `process_thread_exit()`,
then clears the word at `clear_tid` and wakes the waiters on it. It ends in
`sched_terminate()`, which marks the thread `THREAD_DEAD` and gives up the
hart.

A thread never frees itself. The next tick on that hart calls `reap_thread()`
on `hart->dead_thread`, when the stack is no longer in use. It drops the
process reference, the credentials, the pending signals and the kernel stack
with `kstack_free()`. It also frees `io_page`, the event list, the affinity
mask and the `thread_t`.

## Kernel threads and stacks

Every thread, user or kernel, runs on a kernel stack. `kstack_alloc()` takes a
slot from `free_slots`, allocates `KSTACK_PAGES` pages from `pslab` and maps
them `VPROT_RW` in `sys_pmap`. It returns the top of the stack, and the page
below the stack stays unmapped.

The slots lie in one region, `KSTACK_REGION_BASE`. A slot is one guard page
and then the stack, so `KSTACK_SLOT_SIZE` is `(KSTACK_PAGES + 1)` pages.
`KSTACK_NUM_SLOTS` is `CONFIG_MAX_THREADS`, and `free_slots` has exactly that
many bits.

The stack is mapped in every address space. A system call and an interrupt
from user mode run on it. `arch_thread_ctx_load()` writes `thread->kernel_rsp`
into `TSS.rsp_0`, and `amd64_syscall_entry` loads it from `%gs:8`.

The saved context sits at the very top of the stack, where the running code
never reaches. `thread_context()` returns `kstack_top - THREAD_CTX_RESERVE`,
and the live stack grows down from `kernel_rsp`.

A stack that runs off its bottom hits the guard page. The fault handler
cannot run on the broken stack. On the double fault the handler reads CR2, and
`kstack_is_guard()` decides whether to panic with a kernel stack overflow.

A kernel thread has `process == nullptr`, `credentials` set to
`kernel_credentials`, the CR3 of `sys_pmap` and no FPU state. Its entry has the
type `kernel_thread_entry_t`. `arch_thread_setup_kern()` enters it with the
context as the initial `%rsp`. The code installs no return address: no kernel
thread in the tree returns, and a kernel-mode page fault panics.
`init_watchdog` in `kmain.c` is a kernel thread, and so is the idle thread of
each hart, made by `sched_init()` with `sched_idle`. A system call that needs
a process returns `ENOSYS` on one.

## Invariants and traps

- A process flag is only ever set, with one atomic OR. `PROCESS_EXITING`,
  `PROCESS_ZOMBIE`, `PROCESS_COLLECTED` and `PROCESS_EXIT_FINISHED` mark the
  stages of the exit, and `PROCESS_DID_EXEC` marks an exec.
- A process that nobody waits for stays a zombie. It keeps its `process_t` and
  its pid, but no files and no page tables.
- Each thread holds one reference on its process, and each reaped thread drops
  it. The waiter's `process_destroy()` drops the reference that
  `process_create()` returned.
- `process_thread_exit()` returns at once for a thread that already carries
  `THREAD_WORK_EXIT`, so `num_live_threads` cannot fall twice.
- `attach_thread_user()`, `begin_exit()`, `process_dethread()` and
  `process_thread_exit()` change a thread or the count under `process->lock`.
- `process_dethread()` makes the execing thread the only thread and gives it
  the pid as its tid. `exec` waits until `num_threads` is one.
- Signals no longer pick a thread once it carries `THREAD_WORK_EXIT`, and
  `process_has_thread()` reports it as gone.
- `kstack_free()` panics for any address that is not the top of a slot. A
  thread cannot free its own stack, and `reap_thread()` runs on another stack.

## Limits

- There is no thread join in the kernel. `clear_tid` and the futex wake are the
  only notice, and user space waits for it.
- A thread has no name and no detached state. Creation takes the entry, the
  stack top, the TCB, the argument and the address of the clear word.
- `CONFIG_MAX_THREADS` caps kernel and user threads together, and the stack
  slots are static. A thread that finds no slot fails with `ENOMEM`.
- A kernel stack has a fixed size, `CONFIG_KSTACK_PAGES` pages. It does not
  grow, and every reap unmaps its pages.
- Nothing reaps an unclaimed zombie, and only init adopts the children of a
  dying process. There is no subreaper.
- No pid namespace exists, and a pid is never reused below a wrap.
- User space cannot create a kernel thread, and a kernel thread cannot return.
  A subsystem that needs one starts it from its initcall.

## Source

| File | What it holds |
| --- | --- |
| [kernel/include/vex/process.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/process.h#L50) | The `process_t` fields and the `PROCESS_*` flags |
| [kernel/include/vex/thread.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/thread.h#L35) | The `thread_t`, `THREAD_WORK_*` and `thread_context()` |
| [kernel/proc/process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/process.c#L239) | `process_create`, the exit stages, the zombie and the process table |
| [kernel/proc/sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c#L713) | Thread creation, `attach_thread_user`, `reap_thread` and `sched_terminate` |
| [kernel/mm/kstack.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/kstack.c#L72) | The stack slots, the guard page and `kstack_is_guard()` |
| [kernel/arch/amd64/proc/thread.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/thread.c#L21) | The amd64 context, the TSS stack and the CR3 of a thread |
| [kernel/proc/sys_thread.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_thread.c#L21) | `SYS_THREAD_CREATE`, `SYS_THREAD_EXIT`, `SYS_GETTID` and `SYS_TCB_SET` |
| [kernel/proc/user_exit.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/user_exit.c#L8) | `exit_to_user` and the pending work before ring 3 |
| [kernel/proc/sys_process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L730) | `fork`, `exec`, `waitpid` and the claim of a zombie |
| [kernel/init/kmain.c](https://github.com/vex-os/kernel/blob/main/kernel/init/kmain.c#L64) | `spawn_init`, the first thread and `init_watchdog` |
