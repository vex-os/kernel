# Pipes and FIFOs

A pipe is a byte queue with one read end and one write end. `pipe(2)` makes
one that no name leads to. A FIFO is a vnode of a filesystem with the same
queue behind it, and any process that opens the path joins the queue.

Both use `pipe_t`. The queue is a ring of 16 pages, and the pipe holds no page
until the first byte arrives.

## The queue

`pipe_put()` fills the last page and adds a page when that one is full.
`pipe_get()` reads the first page and gives it back as soon as its last byte is
read. An empty pipe therefore holds no page, and a reader that copies slowly
does not pin the whole queue behind one page.

Each page takes one page from the commit limit when the pipe allocates it, and
gives it back when the read drops it. If the charge fails, the write stops and
returns the bytes that it wrote so far, or `ENOMEM` when there are none.

A write of up to `PIPE_ATOMIC_SIZE` (one page) goes in all or nothing. A larger
write waits only for one byte of room, and it then writes as much as the queue
takes. The system call cuts a large write into pieces of `FILE_IO_CHUNK`, so a
piece never exceeds the atomic size.

## Blocking and the ends

A reader waits on the `readable` event while the queue is empty and a writer
still exists. It returns end of file when the last writer closed. A writer
waits on `writable` while the queue has no room. It fails with `EPIPE` and
sends `SIGPIPE` when the last reader closed.

The open and close counts of each side drive those events. The last close of a
reader triggers `writable` and the last close of a writer triggers `readable`.
`O_NONBLOCK` turns both waits into `EAGAIN`.

`poll` reports `POLL_READABLE` when a byte waits, `POLLHUP` when no writer is
left, `POLLERR` when no reader is left, and `POLL_WRITABLE` only when a whole
atomic write fits. `FIONREAD` returns the bytes in the queue. `seek` always
fails with `ESPIPE`.

## FIFOs

A FIFO node starts with no queue. The first open attaches a `pipe_t` to the
node, and the last close detaches it and frees the data, as POSIX requires.

An open that waits for the other end uses the `partner` event and two counters
of opens that ran so far. A reader that opens alone waits for a writer. A
writer that opens alone waits for a reader, and with `O_NONBLOCK` it fails with
`ENXIO` instead. A reader with `O_NONBLOCK` goes on at once. An open for both
directions never waits.

The counter of opens, and not only the count of open files, is the reason that
the wait ends. A partner that came and closed again still satisfies it. The
check runs under the queue lock, so the open that ends the wait cannot pass
between the check and the sleep.

## The pipe filesystem

`pipefs` is an internal filesystem, in the sense of
[VFS](../vfs.md): no kernel command line can mount it. Its root is a
directory, and `pipe(2)` adds a FIFO vnode below it with a queue attached.
Both ends of the pipe are ordinary `file_t` objects on that vnode, so the file
code, the file descriptor table and `poll` need no pipe-specific path.

The queue carries the effective user and group of the process that created the
pipe. A FIFO shows the owner of its node instead.

## Why it is done this way

- **Pages, not a ring buffer.** An empty pipe costs nothing, and a slow reader
  returns each page as soon as it is done with it.
- **One atomic size.** A writer of a small message cannot be interleaved with
  another writer. A larger write is a stream by design.
- **The same code for a pipe and a FIFO.** Only the attach and the open
  handshake differ.
- **The open counters.** A wait for a partner must end even when that partner
  opened and closed before the waiter ran again.

## Limits

- A pipe holds at most 16 pages, which is 64 KiB.
- There is no `splice(2)`, no packet mode and no way to read one message at a
  time.
- A call can block on a full queue while another thread of its process closes
  the reader. The writer then wakes with `EPIPE`, as POSIX has it.

## Source

- [pipe.c](https://github.com/vex-os/kernel/blob/main/kernel/ipc/pipe.c#L39) — the queue, the events, the open handshake and `pipefs`
- [pipe.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/pipe.h) — `pipe_create`
- [sys_file.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L913) — `pipe(2)`
- [poll.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_poll.c) — the `poll` and `ppoll` calls
