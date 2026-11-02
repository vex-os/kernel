# IPC

Processes talk to each other through three mechanisms:

- a pipe or a FIFO, a byte stream with one read end and one write end.
- a futex, a wait on a 32-bit word that both sides map.
- a shared mapping, memory that several processes read and write.

There is no socket layer, no message queue and no semaphore.

A named shared memory object, as `shm_open` makes it, is not a kernel object
either. The C library uses a file on a tmpfs that userspace mounted at a fixed
path. The [shared memory](shmem.md) page gives the detail.

| Mechanism | Made of | Blocks on | Lives while |
| --- | --- | --- | --- |
| [Pipes and FIFOs](pipes.md) | a `pipe_t` behind a vnode | events on the pipe | the vnode |
| [Futexes](futexes.md) | a queue in one of 256 buckets | the scheduler | the waiting call |
| [Shared memory](shmem.md) | a tmpfs file with no name | no wait, faults on touch | a mapping holds it |

## What they share

A blocked read or write, and a contended futex, sleep through the scheduler.
The tick wakes a sleeper whose deadline passed, so a timeout lands on a tick
and not earlier.

Pipes and shared mappings count their pages against the
[commit limit](../memory/commit.md). A pipe page takes one page while the
queue holds it. A shared file takes its whole size when the mapping appears. A
futex takes none, as its waiter lives on the stack of its thread.

Only pipes and shared mappings carry an access check. A pipe holds the
effective user and group of the process that made it. A shared file is made
with the credentials of the kernel and reached only through a mapping that
`mmap` already accepted. A futex trusts the mapping of the caller.

Pipes reuse the file path of the [VFS](../vfs.md), so `read`, `write`, `poll`
and `ioctl` reach the same vnode ops as any other file. A futex and a shared
mapping have their own system call paths.

## Pages

- [Pipes and FIFOs](pipes.md) — the ring of pages, the open handshake and
  `poll`.
- [Futexes](futexes.md) — the private and the shared key, the waiter and the
  timeout.
- [Shared memory](shmem.md) — the unnamed file, the commit charge and the
  lifetime.
