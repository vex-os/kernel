# Scheduler

Each hart runs one thread at a time. The scheduler chooses the next thread and
gives the current thread a timeslice. It uses one run queue per hart, a
per-thread affinity mask and the nice value of the process. All threads use
`SCHED_OTHER`.

The scheduler has two jobs. It shares each hart among the runnable threads, so
one thread cannot hold a hart forever. It also keeps a thread on a hart that
the affinity mask allows.

## Run queues

Each hart has a `runqueue_t` in `runqueues[]`. It holds a `lock`, a `head` and
a `tail` for an intrusive doubly linked list of `thread_t`. `num_queued` is
an atomic count that an idle or stealing hart reads without the lock.

A thread in `THREAD_RUNNABLE` is on exactly one run queue. A thread in
`THREAD_RUNNING` is on a hart. A thread in `THREAD_BLOCKED` is on no queue.
`THREAD_DEAD` is a thread that must leave its stack before the kernel frees it.

`sched_enqueue()` wakes a blocked thread. It calls `choose_hart()`, marks the
thread `THREAD_RUNNABLE` and puts it at the tail of that hart's queue.

`choose_hart()` returns `bound_hart` first. Otherwise it prefers `last_hart`,
the hart that ran the thread last. It keeps that hart when the thread may run
there. The hart must also be idle, or the thread must still hold its
`hart_lock`. Otherwise it looks for an idle hart that the affinity mask
permits. If every permitted hart is busy, it still returns one of them.

At a tick, `rq_take_nolock()` walks the queue from `head`. It takes the first
thread that `thread_may_run_on()` permits and whose `hart_lock` and
`sched_lock` it can acquire. A held `hart_lock` shows that another hart still
uses the stack of the thread. The scheduler skips such a thread.

A hart that would otherwise idle calls `rq_steal()`. It tries the other queues
in a rotating order, starting after its own index. When no thread is
available, the hart clears `HART_FLAG_ACTIVE`, loads `idle_thread` and stops.
`sched_idle()` sends itself a scheduler IPI when its queue becomes non-empty.

```
wake  sched_enqueue --> choose_hart --> rq_push_nolock
                                         |
tick  rq_take_nolock <-------------------+
           | empty
           v
       rq_steal --> next thread, else idle_thread
```

The queue does not order threads by priority. It is first in, first out. A
thread that becomes runnable goes behind the threads that are already queued.

## Priorities

Vex does not have priority queues. The nice value of a process changes only
the timeslice of its threads. `process->nice` holds the value, and `thread_t`
has no nice field.

`CONFIG_SCHED_TIMESLICE` sets the base timeslice in microseconds. The default
is 5000, so `SCHED_TIMESLICE_NS` is 5 ms. `init_nice_timeslices()` builds the
table for the 40 levels from `NICE_MIN` to `NICE_MAX`.

A step down from 0 multiplies the timeslice by 5/4. A step up from 0
multiplies it by 4/5. The result stays between `SCHED_TIMESLICE_MIN_NS`
(100000 ns) and `SCHED_TIMESLICE_MAX_SCALE * SCHED_TIMESLICE_NS`, which is
four times the base. `sched_timeslice_for_nice()` clamps a value outside the
range to the nearest end.

A kernel thread has no process. It always receives the base timeslice.
`getpriority()` returns `NICE_BIAS - nice`, with `NICE_BIAS` equal to 20.
`setpriority()` clamps the value and checks ownership or `CAP_SYS_NICE`. A
thread without the capability cannot lower a nice value below the floor that
`RLIMIT_NICE` sets.

All threads use `SCHED_OTHER`. `sched_setscheduler()` rejects any other
policy. It and `sched_setparam()` reject a nonzero `sched_priority`.

## Affinity

`thread_t.affinity` points to `SCHED_AFFINITY_WORDS` words of 64 bits. Each
bit is one hart. A null pointer means that every hart is allowed.
`thread_may_run_on()` checks `bound_hart` first, then tests the bit of the
current hart. `bound_hart` is an internal constraint for a thread that must
stay on one hart.

`thread_reserve_affinity()` allocates a mask with every bit set if the thread
has none. `thread_set_affinity()` stores the new words in place and calls
`affinity_changed()`. It does not wait for the thread to move.

The words are atomic, but a mask update is not one transaction. A reader can
see a mix of old and new words. That mix only changes where the thread goes
next, and the next check corrects the choice.

`affinity_changed()` moves a runnable thread that sits on a disallowed hart.
It removes the thread from that queue and wakes it on a permitted hart. For a
running thread, it sends an IPI to the hart. At the next tick the thread
becomes blocked and the scheduler enqueues it again.

The affinity system calls apply `online_word()` to the user mask.
`sched_setaffinity()` fails with `EINVAL` when the mask has no online hart.
`sched_getaffinity()` reports only online harts. `sched_setaffinity()` with a
tid of 0 gives each thread of the process its own mask.

## Preemption

A thread has `preempt_count`. `preempt_disable()` raises the count and
`preempt_enable()` lowers it. `spinlock_acquire()` and `spinlock_release()`
call both functions. A thread with a nonzero count must not be preempted.
Another thread on the same hart could spin on the lock.

When the timeslice expires, `sched_irq_handler()` runs. If a running thread has
a nonzero `preempt_count`, the handler sets `need_resched` and rearms the
timer. It does not switch. `preempt_enable()` calls `sched_yield()` when the
count reaches zero, `need_resched` is set and interrupts are enabled.

A thread that waits calls `sched_dequeue()` and then `sched_yield()`.
`sched_dequeue()` marks the thread `THREAD_BLOCKED` and takes a runnable
thread off its queue, but it does not switch. `sched_yield()` raises a
self-IPI. The thread continues only after the scheduler picks it again. It
requires interrupts enabled and `preempt_count` equal to zero.

For a live thread, the handler saves its context at `thread_context(curr)`.
It takes the next thread from a queue, or the idle thread, loads the chosen
context and calls `sched_enter()`. The assembly sets
`%rsp` to that context. It releases the old `hart_lock` only after the stack
change. Another hart can then resume the old thread.

A wakeup does not replace the current thread at once. The scheduler sends an
IPI only to an idle hart. The current thread then runs until its next tick or
until it yields.

## Invariants and traps

- A `THREAD_RUNNABLE` thread is on exactly one run queue. The scheduler changes
  its `state` under `sched_lock` and its queue links under the runqueue lock.
- When the scheduler holds both, it takes `sched_lock` before a runqueue
  lock. With a runqueue lock held, it only tries `hart_lock` and
  `sched_lock`.
- A thread taken from a run queue runs only after `rq_take_nolock()` acquires
  its `hart_lock`.
- `sched_enter` releases the `hart_lock` of the previous live thread only
  after the stack switch.
- The timeslice is not a hard limit. A thread with a spinlock can hold a hart
  past the end of its timeslice.
- A mask read can mix two updates. A running thread can stay on a disallowed
  hart until the next tick.
- The kernel does not free a dead thread on its own stack. The next tick reaps
  `hart->dead_thread`.

## Limits

- `SCHED_OTHER` is the only policy. The constants `SCHED_FIFO`, `SCHED_RR`,
  `SCHED_BATCH` and `SCHED_IDLE` exist in the ABI, but the calls reject them.
- Nice changes the length of a timeslice only. It does not change queue order
  or select a different queue.
- Nice belongs to a process, not to a thread. Every thread of a process gets
  the same nice value.
- There is no periodic load balance. A TODO in `sched.c` records this limit.
  A busy hart keeps its queue, and only an idle hart steals from it.
- The kernel does not provide a user interface for `bound_hart`. Affinity is
  the only user control of placement.
- `SCHED_AFFINITY_WORDS` follows `MAX_HARTS` at compile time. A larger machine
  needs a new build.

## Source

| File | What it holds |
| --- | --- |
| [sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c#L45) | the run queues, hart choice, timeslices, the tick and affinity changes |
| [sched.S](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/sched.S#L5) | `sched_enter`, the context switch and the delayed `hart_lock` release |
| [thread.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/thread.c#L137) | the save and load of the amd64 thread context |
| [sched.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/sched.h#L39) | thread creation, enqueue and the affinity interface |
| [sys_sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L190) | the nice, policy and affinity system calls |
| [thread.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/thread.h#L35) | thread state, affinity, run queue links and `preempt_count` |
| [spinlock.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/spinlock.h#L43) | spinlocks hold off preemption and call `preempt_enable()` on release |
