# Futexes

A futex is a wait on a 32-bit word that several threads or processes map. The
kernel holds no state while nobody waits, so the fast path of a lock stays in
user space. Only contention enters the kernel.

`SYS_FUTEX_WAIT` takes the address, the value that the caller expects, an
optional relative timeout and the flags. `SYS_FUTEX_WAKE` takes the address,
the number of waiters to wake and the flags. The only flag is `FUTEX_SHARED`.

## The key

The kernel identifies a futex by a key of two numbers: a `vmspace_t*`, which
is null for a shared futex, and an address.

- A private futex keys on the `vmspace_t` of the caller and the virtual
  address. Two threads of one process reach the same key through the same
  mapping, and another process never does.
- A shared futex keys on the physical address of the page. Two processes that
  map the same page reach the same key even when the page is at a different
  virtual address in each process.

The wait pins the page with `vms_pin()` and reads the word through the direct
map. A private wait unpins the page before it sleeps. A shared wait keeps the
pin until it is done, because the physical address is part of the key and the
frame must not move.

The keys spread over 256 buckets, with the top 8 bits of a multiplication by
the golden ratio as the index. Each bucket holds one lock and one list.

## Wait

The wait runs these steps under the lock of the bucket:

1. It reads the word. A value other than the expected one fails with
   `EAGAIN`.
2. A deadline that already passed fails with `ETIMEDOUT`.
3. It puts a `futex_waiter_t` on the list. The waiter lives on the stack of
   its thread and is linked in arrival order.
4. It removes the thread from the scheduler, asks for a wake at the deadline,
   and yields.

The waker stores the new word before it calls the wake, and the wake takes the
bucket lock. A store either happens before the read of step 1, or it happens
after the waiter is linked, where the waker finds it.

On the way back the thread takes the bucket lock again. A waiter that is still
on the list timed out or lost a wake race. It unlinks itself.

The call then reports these results, in this order. It reports `0` when the
waker took the waiter, `EINTR` when work is pending, `ETIMEDOUT` when the
deadline passed, and `0` for a spurious wake. The caller looks at the word
again in every case.

## Wake

The wake takes the bucket of the key and walks the list from the head. The
waiters of one futex leave in the order that they arrived. It re-enqueues up to
`count` threads whose key matches, and it returns the number. A shared wake
pins the page first to learn the physical address, then unpins it.

## Why it is done this way

- **The waiter lives on the stack.** A contended wait allocates nothing, and
  the bucket lock is the only thing that keeps the waiter valid.
- **The word is checked once, under the lock.** A store that happens before
  the check is seen by it. A store that happens after finds the waiter on the
  list.
- **The shared key is physical.** Two processes rarely map a shared page at
  the same virtual address.
- **The timeout is a scheduler deadline.** The tick already wakes threads
  whose deadline passed, so no separate timer queue exists.

## Limits

- There is no priority inheritance, no `FUTEX_ROBUST` support and no
  owner-death handling.
- Only `FUTEX_WAIT` and `FUTEX_WAKE` exist. There is no requeue, no bitset
  wait and no `FUTEX_WAITV`.
- A large `count` wakes every waiter on the key. There is no separate
  broadcast flag.

## Source

- [futex.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/futex.c#L20) — the buckets, the waiter and the two operations
- [futex.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/futex.h) — the interface
- [sys_futex.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_futex.c#L21) — the two system calls
- [sched.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/sched.h) — `sched_wake_at()` and `sched_wake_cancel()`
