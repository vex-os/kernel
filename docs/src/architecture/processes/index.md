# Processes

A process holds an address space, a table of descriptors and one signal
disposition table. A thread is the unit that the scheduler runs. A process
starts with one thread, and `sys_thread_create` adds more.

`kernel/proc/` creates and reaps processes, and creates, schedules and reaps
their threads. Three pages follow:

| Page | Owns |
| --- | --- |
| [Scheduler](scheduler.md) | one `runqueue_t` per hart and the tick |
| [Threads](threads.md) | the `thread_t`, its kernel stack and its process |
| [Signals](signals.md) | the `pending_signal_t` queues and the handlers |

## Harts

A hart is one hardware thread of the CPU. `harts[MAX_HARTS]` holds one
`hart_local_t` per hart. `MAX_HARTS` is `CONFIG_MAX_HARTS` with `CONFIG_SMP`,
and `1U` without it. `num_harts` counts the harts that `arch_enumerate_harts`
registered.

Hart 0 is the boot hart, and `boot_hart()` returns it. The kernel counts a
hart past `MAX_HARTS` in `ignored_harts` and leaves it unused. `init_harts`
starts the others with `arch_start_hart`.

A secondary hart waits in `hart_park` on `hart->bringup`. `init_harts` marks a
hart that `cpucaps_merge` refused with `HART_BRINGUP_PARK`, and `init_hart_boot`
sets `HART_BRINGUP_GO` for the others. A parked hart never runs a thread.
[Boot and init](../boot.md) give the full order.

Every online hart runs `bring_online`, so the `initcalls_smp_*` stream runs on
each hart, the boot hart included. `this_hart()` returns
`this_thread()->last_hart`: the hart that runs the thread now, or the last hart
that ran it.

Each hart has one run queue, `runqueues[hart->index]`. Every online hart also
has one idle thread that runs `sched_idle`. A hart with `HART_FLAG_ONLINE` set
and `HART_FLAG_ACTIVE` clear is idle, as `hart_is_idle` reports. Only a hart
that would otherwise idle takes work from another queue, in `rq_steal`.

A thread with no mask of its own, `thread->affinity == nullptr`, may run on
every hart. A thread created from another thread inherits its mask. The first
`sys_setaffinity` gives a thread without one a mask of `SCHED_AFFINITY_WORDS`
words, with one bit per hart. A thread with `bound_hart` set runs only on that
hart. Only the uACPI worker sets it.

## What they share

The three mechanisms meet at `thread->pending_work`, a word of `THREAD_WORK_*`
bits. A thread reads it on the way back to user mode. `exit_to_user` acts on it
in a fixed order: `THREAD_WORK_EXIT` calls `sched_terminate`,
`THREAD_WORK_STOP` calls `job_park`, and `THREAD_WORK_SIGNAL` calls
`signal_deliver`. The scheduler never runs that work itself.

The pid and the tid come from one counter, `process_alloc_id`. The first thread
of a process takes its pid as its tid, in `attach_thread_user`. A tid therefore
never names a thread of another process.

The process keeps the resources. A thread keeps its process alive.
`attach_thread_user` takes a reference with `process_incref`, and
`process_thread_reaped` drops it when the scheduler frees the thread. When the
scheduler reaps the last thread, the process gives up its `vmspace` with
`process_set_vmspace(process, nullptr)`. It then sets `PROCESS_ZOMBIE` and
sends `SIGCHLD` to the parent. It stays a zombie until a waiter claims it with
`process_claim` and calls `process_destroy`.

The lock order is `signal_actions->lock`, then `process->lock`, then a
`thread->sched_lock`, then the run queue lock. `rq_take_nolock` holds the run
queue lock and only tries the thread locks. Elsewhere `sched_lock` comes before
the run queue lock. A held `hart_lock` means another hart is still on that
thread's stack.

A thread changes `state` only under its `sched_lock`. A wait follows one
protocol. Register as a waiter under the lock of the waker, and call
`sched_dequeue` before that lock is released. Then call `sched_yield`, and
check the condition again. Without the first two steps, a wakeup gets lost.

## Pages

- [Scheduler](scheduler.md) — the run queue of each hart, `nice`, affinity and
  the tick.
- [Threads](threads.md) — `process_create`, the kernel stack, kernel threads
  and the end of a process.
- [Signals](signals.md) — the pending queues, the mask, delivery and job
  control.

## Limits

- There is no hart hotplug. `num_harts` and the set of parked harts never
  change after `init_harts`.
- The only policy is `SCHED_OTHER` with priority 0. `sys_setscheduler` refuses
  every other policy, and `sys_getschedparam` reports priority 0. `sys_sched.c`
  marks `SCHED_FIFO` and `SCHED_RR` as `UNDONE`.
- A hart takes work from another queue only when its own queue has nothing it
  can run. The `TODO` in `choose_hart` notes that the tick does not balance
  queues between busy harts.
- `nice` is a field of `process_t`, not of `thread_t`. Threads of one process
  share one nice value, so their timeslices have the same length.
- The kernel cannot use more than `MAX_HARTS` harts. A machine with more harts
  runs on the first `MAX_HARTS` that the kernel registers.
