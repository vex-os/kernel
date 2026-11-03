# Signals

A signal is an asynchronous notification to a process or to one thread. It
reports a fault, a terminal event or a request from another process. The
kernel queues it and chooses a thread that can take it. On the way back to
ring 3, it builds a handler frame for a caught signal. The ABI follows POSIX:
there are 64 signal numbers, and `SIGRTMIN` starts the real-time range.

The kernel sends a signal for a fault, a timer or a terminal event. Another
process sends one through `kill(2)`, `tgkill(2)` or `sigqueue(2)`.

## Delivery

A sender calls `signal_send()`, `signal_queue()` or `signal_raise()`.
`signal_queue()` serves `sigqueue(2)`, and `signal_raise()` serves the timer.
`signal_send()` allocates a `pending_signal_t`. When memory is short, a
standard signal still arrives, with `SI_KERNEL` if no entry waits for it.

Only a signal from `SIGRTMIN` up queues more than one entry in one queue. A
standard signal sets one bit, and a second send to the same queue merges with
the pending entry.

`queue_signal()` takes both locks, then chooses the thread. It drops a signal
as ignored only when some thread could take it now. A blocked signal stays
pending, because its action may change before it is unblocked.

`choose_thread_locked()` prefers the caller, then walks `process->threads`. It
skips a thread that blocks the signal or has `THREAD_WORK_EXIT`. A signal for
one `tid` goes to the queue of that thread.

`recalc_locked()` sets `THREAD_WORK_SIGNAL` when a thread has a deliverable
signal. `exit_to_user()` handles that bit before it returns to ring 3.
`sched_kick()` makes a running thread pass that point.

`deliver()` takes the lowest signal number first. It looks at the queue of the
thread, then at the queue of the process. It removes the entry, or clears the
bit, under `signal_actions->lock`.

A signal with the default action runs the action in `default_actions[]`. The
actions are terminate, core, ignore, stop and continue. A signal with a
handler gets a `sigframe_t`.

The init process is special. Only a signal it handles reaches it, and `SIGKILL`
spares it, because its death stops the system. A fault that resolves to the
default action still kills it, through `signal_force()`.

`signal_force()` raises a fault signal for the calling thread. It turns an
ignored action or a blocked signal into the default action. A fatal default
action kills the process at once.

```
kill(2) -> signal_send -> queue of the process or of a thread
                              |
                        queue_signal sets
                        THREAD_WORK_SIGNAL
                              |
                   exit_to_user, on the way to ring 3
                              |
                signal_actions->actions[sig - 1]
                     /                     \
              SIG_DFL                  a handler
        stop, kill or ignore     arch_signal_setup
```

## Masks

`sig_blocked` is per thread. `sigset_t` is a 64-bit mask, and `sigbit(sig)`
maps a signal number to its bit. `signal_mask()` applies `SIG_BLOCK`,
`SIG_UNBLOCK` or `SIG_SETMASK`.

`SIGKILL` and `SIGSTOP` are never blocked. `sigaction(2)` also removes them
from `sa_mask`.

`signal_replace_mask()` saves the mask for a wait, and `signal_restore_mask()`
puts it back. A handler restores the saved `uc_sigmask` through `sigreturn`.

A handler runs with the old mask, `sa_mask` and its own signal blocked.
`SA_NODEFER` keeps its own signal unblocked. `SA_RESETHAND` resets the action
to `SIG_DFL`.

`fork` copies the mask and the alternate stack. `exec` keeps the mask and the
pending signals of the thread and the process. It clears the alternate stack
and resets every handler except `SIG_IGN`.

## Waiting

`sigsuspend()` replaces the mask and sleeps. A signal of the set ends the
sleep and runs its handler on the way out. A stop does not end the wait. The
process stays in the call when it continues.

`sigtimedwait()` takes a pending signal of the set without running its
handler. It unblocks the wanted set for the time of the wait, and returns
`-EAGAIN` on a timeout.

`sigpending()` reports the set that is pending and blocked. `waitpid()` and
`waitid()` wait on `process->child_exited`.

`wait_child()` takes a zombie, or a stop or continue report, from
`job.report`. `WNOHANG` returns zero when no child reports. `SA_NOCLDSTOP`
suppresses `SIGCHLD` for a stop or a continue, but not the wait report.

An interruptible wait that gets a signal returns `-ERESTART`. The system call
entry rewinds the call and sets `thread->syscall_restart`. A caught handler
without `SA_RESTART` turns it into `-EINTR`. Otherwise the call runs again.

## Job control

`job_control_t` holds the state of one process: `JOB_RUNNING` or
`JOB_STOPPED`, the stop signal, two report bits and the `resume` event.
`process->lock` protects the state. The report bits change with atomic
operations.

`job_stop()` sets `JOB_STOPPED`, records `stop_signal`, sets
`JOB_REPORT_STOPPED` and posts `THREAD_WORK_STOP` to every live thread.
`job_park()` blocks a thread until the process runs again.

`SIGCONT` acts when it is sent. It purges the stop signals. A stopped process
clears `JOB_REPORT_STOPPED`, sets `JOB_REPORT_CONTINUED` and triggers
`resume`. A stop signal purges a pending `SIGCONT`.

`SIGSTOP`, `SIGTSTP`, `SIGTTIN` and `SIGTTOU` stop by default. A terminal
sends `SIGTTIN` or `SIGTTOU` to a background group. A refused `SIGTTIN`, or an
orphaned group, fails the access with `-EIO`. A refused `SIGTTOU` lets the
access through.

`process_group_orphaned_locked()` says that no member has a parent outside the
group in the same session. A terminal stop signal does not stop an orphaned
group, because nobody could continue it.

An exiting session leader sends `SIGHUP` and `SIGCONT` to the foreground
group. An orphaned group with a stopped member also gets `SIGHUP` and
`SIGCONT`.

`kill(2)` sends to a process, a group or every process. `kill_all` skips the
sender and init. `may_signal()` requires `CAP_KILL` or a matching uid.

## Invariants and traps

- `signal_actions->lock` is always taken before `process->lock`.
  `process_group_lock` nests outside both.
- The kernel looks up a target thread under the locks that queue the signal.
  The thread therefore cannot be reaped in between.
- The kernel drops a signal as ignored only when a thread could take it now.
  A signal that is blocked at send time stays pending.
- A handler frame is never built inside a system call.
  `signal_deliver_uncaught()` leaves a caught signal pending for the return.
- The process dies when `arch_signal_setup()` cannot build a frame.
- A fault signal must not return to the faulting instruction. The kernel turns
  an ignored action or a blocked signal into the default action, which kills.
- `signal_take()` removes a pending signal, and no handler runs for it.
- A standard signal has at most one entry in a queue. Each real-time entry
  takes memory, and `RLIMIT_SIGPENDING` limits `sigqueue(2)` only.
- A zombie goes to exactly one waiter. `process_claim()` decides which one.

## Limits

- There is no `sigwait()`, no `sigwaitinfo()` and no `signalfd`. The only
  synchronous wait for a signal is `sigtimedwait()`.
- `SA_SIGINFO` passes the flag check, but no kernel code reads it. The frame
  always holds a `siginfo_t`, and the kernel always calls `sa_handler` with
  three arguments.
- `SA_NOCLDWAIT` passes the flag check, but no kernel code reads it. A zombie
  stays until a wait claims it.
- `DEFAULT_CORE` terminates the process like `DEFAULT_TERM`. The kernel writes
  no core file.
- `RLIMIT_SIGPENDING` counts the process queue and the thread queues together.
  `kill(2)`, `signal_send()` and `signal_raise()` do not check it.
- A real-time signal can be lost when no entry is available and an entry of
  the same signal waits already.
- `SI_TIMER`, `SI_MESGQ` and `SI_ASYNCIO` are defined, but no kernel path
  sends them.

## Source

| File | What it holds |
|------|---------------|
| [signal.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/signal.h#L112) | the action and pending objects, and the signal API |
| [process.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/process.h#L38) | `job_control_t` and the job control calls |
| [thread.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/thread.h#L80) | the per-thread mask, queue and alternate stack |
| [signal.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/signal.c#L378) | queueing, thread choice, delivery and masks |
| [amd64 signal.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/signal.c#L72) | the handler frame and `sigreturn` |
| [sys_signal.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L121) | the signal system calls and the permission check |
| [job.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/job.c#L54) | stop, continue, park and wait reports |
| [pgrp.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/pgrp.c#L207) | groups, sessions and orphan detection |
| [process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/process.c#L529) | process exit, kill and the `SIGCHLD` note |
| [user_exit.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/user_exit.c#L8) | delivery on the way to ring 3 |
| [amd64 syscall.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/syscall.c#L24) | the syscall rewind and `-EINTR` |
| [sys_process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L602) | `waitpid(2)` and `waitid(2)` |
| [tty_interface.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/tty_interface.c#L428) | terminal foreground checks and job signals |
