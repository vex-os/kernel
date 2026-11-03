# mlibc sysdeps plan

Status: draft, ready to start. Base: mlibc `vex-sysdeps` at `27aa0555`, kernel
`6312e19`, 2026-11-03.

Scope: `mlibc/sysdeps/vex/` and `mlibc/abis/vex/` in the mlibc submodule of
the bootstrap repository. Bootstrap moves the submodule pointer.

The kernel plan is [kernel-plan.md](kernel-plan.md). This plan needs no new
kernel call. If a sysdep needs one, add the kernel item first.

## State of the tree

- The mlibc submodule follows the `vex-sysdeps` branch of `vex-os/mlibc`.
  The bootstrap repository records which commit it uses.
- The tag list is `VexSysdepTags` in
  `sysdeps/vex/include/mlibc/sysdeps.hpp`. It holds 94 tags. A tag that is not
  in the list has no sysdep, and mlibc fails the call with `ENOSYS`.
- `sysdeps/vex/meson.build` declares the `posix` option alone. A tag that sits
  in a block that is off has no name at all, so it can neither go into the list
  nor be implemented. See "Options that gate a sysdep".
- The signatures are in
  `options/internal/include/mlibc/sysdep-signatures.hpp`. The tags and their
  comments are in `options/internal/include/mlibc/sysdep-tags.hpp`.
- The implementations are in `generic/sysdeps.cpp`, `generic/ioctl.cpp`,
  `generic/mount.cpp` and `generic/entry.cpp`.
- The ABI values live in `abis/vex/` (real files) and `abis/linux/` for the
  calls that follow the Linux ABI. The files in
  `sysdeps/vex/include/abi-bits/` are Git symlinks to them.
- The build is `recipes/mlibc/recipe` of bootstrap. It passes
  `-Dvex_kernel_headers=<kernel>/kernel/include/usr,<kernel>/kernel/arch/amd64/include/usr`.
  mlibc reads the kernel headers from the kernel tree, so the kernel ABI is
  the source of truth.
- The thread code that calls the sysdep tags is
  `options/internal/generic/threads.cpp`. It prepares the stack, calls
  `Clone`, writes the tid into the TCB, and wakes a futex.
- **The tree does not compile against the current kernel.** It uses
  `__k_SYS_DUP2`, `__k_SYS_DUP3` and `__k_SYS_SLEEP`. The kernel removed these
  numbers. Phase 0 repairs the tree.
- A Git checkout on Windows may write a symlink as a small text file with the
  target path. Build mlibc in a Linux environment, or in WSL, where the
  symlinks are real.

## Phase 0: repair the tree

Do this phase first. Nothing else builds until it is done.

| Site | Now | Change |
| --- | --- | --- |
| `Sysdeps<Dup2>` | `__k_SYS_DUP2`, `__k_SYS_DUP3` | `SYS_DUP(fd, newfd, flags)`. Two equal numbers return 0 after a `fcntl(F_GETFD)` check, as `dup2` has it. `flags` is 0 or `O_CLOEXEC` |
| `Sysdeps<Sleep>` | `__k_SYS_SLEEP` | `SYS_NANOSLEEP(CLOCK_MONOTONIC, 0, &request, &remaining)` |
| `Sysdeps<Waitpid>` | 3 arguments, then `memset(ru)` | Pass `ru` as the fourth argument. The kernel fills it, and writes nothing when the pointer is null |
| `Sysdeps<Fcntl>` | 5 commands | Add the lock commands, `struct __k_flock` and the `F_OFD_*` rules |
| `abis/vex/fcntl.h` | `O_TMPFILE` 0x20000, locks 1001 to 1008 | `__k_O_TMPFILE` (0x0400), `__k_F_GETLK` to `__k_F_OFD_SETLKW` (6 to 11) |
| `abis/vex/fcntl.h` | "The kernel ignores these flags" | Check each flag against `usr/vex/fcntl.h`. `O_TMPFILE`, `O_CLOEXEC` and `O_NONBLOCK` work now |
| `abis/vex/vm-flags.h` | 4 `MAP_*` flags | Add `MAP_NORESERVE`, `MAP_STACK`, `MAP_POPULATE`, `MAP_FIXED_NOREPLACE`, and the `MADV_*` and `MCL_*` values |
| `abis/vex/wait.h` | "The kernel has no job control" | Remove the comment. Use `__k_WUNTRACED`, `__k_WCONTINUED` and the `__k_W_*` macros |
| `Sysdeps<Uname>` | `domainname` is `"skibidisigma6767"` | `(none)`, as the decision of the syscall plan says |
| `abis/vex/signal.h`, `abis/vex/stat.h` | No layout checks | Add `static_assert`s against `__k_sigaction`, `__k_siginfo`, `__k_sigset_t` (8 bytes), `__k_stat`, `__k_rusage`, `__k_rlimit` |

Notes:

- `SYS_DUP` is the only descriptor copy call. `dup` keeps
  `fcntl(F_DUPFD)`, and `dup2` uses `SYS_DUP` with no flags.
- `SYS_NANOSLEEP` takes `(clock, flags, request, remaining)`. mlibc's `Sleep`
  is a relative monotonic call, so `clock` is `CLOCK_MONOTONIC` and `flags`
  is 0. The kernel writes `remaining` only when a signal ends the wait.
- The kernel's `waitpid` takes `(pid, status, flags, rusage *)`. The old
  sysdep ignored the last argument.
- The lock numbers 6 to 11 are the kernel's, not the Linux values 6, 7, 8
  and 37 to 39. Take them from `usr/vex/fcntl.h`.
- `O_NOCTTY` is `0x0400` in `abis/vex/fcntl.h`, and `__k_O_TMPFILE` is also
  `0x0400`. The kernel does not ignore that bit: `file_open` takes the
  `O_TMPFILE` path on it (`kernel/fs/file.c` line 207), so an `open` with
  `O_NOCTTY` would make a file with no name. Give `O_NOCTTY` a bit of its own,
  or clear it in `Openat` and `Open` before the call. The flags above it,
  such as `O_DSYNC` at `0x0800`, do not collide.
- `__k_rusage` and `__k_rlimit` come from `usr/vex/resource.h`, but
  `abi-bits/resource.h` is a symlink to the shared `abis/linux/resource.h`, as
  are `rlim_t.h`, `sched_param.h`, `poll.h`, `sigval.h`, `sigevent.h` and
  `time.h`. A layout check under Vex needs an `abis/vex/resource.h` of its own
  and a symlink that points at it. Do not add the checks to the Linux file,
  which other targets use.

Done when the mlibc recipe builds against the current kernel, and a
hello-world program runs on the test image.

## How to add a sysdep

1. Add the tag name to `VexSysdepTags`. Only a tag of a block that is on has a
   name; see "Options that gate a sysdep".
2. Copy the signature from `sysdep-signatures.hpp`. Do not invent one.
3. Define `mlibc::Sysdeps<Tag>::operator()` in `generic/sysdeps.cpp`, or in a
   file of its own for a group such as threads or signals.
4. Call through `vex::syscall` with `vex_syscall_errno` or
   `vex_syscall_result`. Return an errno, not -1.
5. Keep the tag out of the list while the kernel lacks the call. A tag in the
   list promises that the call exists.
6. Build with the recipe and run a program on the test image.

## Options that gate a sysdep

`sysdeps/vex/meson.build` line 1 declares
`sysdep_supported_options = { 'posix': true }`. That one entry is the whole
set of options that Vex turns on.

`meson.build` turns each option into a `__MLIBC_*_OPTION` macro (lines 360 to
383). The tag declarations in `sysdep-signatures.hpp` sit inside those blocks.
A tag of a block that is off is not declared at all, so it cannot go into
`VexSysdepTags` and cannot be implemented: there is no type to name.

| Block | Macro |
| --- | --- |
| default, always on | none |
| posix | `__MLIBC_POSIX_OPTION` |
| linux | `__MLIBC_LINUX_OPTION` |
| linux-epoll, linux-timerfd, linux-signalfd, linux-eventfd, linux-reboot, linux-wrappers | `__MLIBC_LINUX_*_OPTION` |
| bsd, bsd-procdesc | `__MLIBC_BSD_OPTION`, `__MLIBC_BSD_PROCDESC_OPTION` |
| glibc | `__MLIBC_GLIBC_OPTION` |

The `linux` block is all or nothing. It adds the whole `options/linux` API
surface, it fails the build unless `linux_kernel_headers` names a Linux header
tree (`meson.build` lines 394 to 402), and it turns `linux-epoll`,
`linux-timerfd`, `linux-signalfd`, `linux-eventfd` and `linux-reboot` on by
default (lines 365 to 369). Vex must not turn the option on to reach one tag.

These tags of this plan sit in the `linux` block:

| Tag | Phase |
| --- | --- |
| `Syncfs` | 3 |
| `Umount2`, `Getcpu` | 5 |
| `Sysinfo` | 7 |
| `Mount`, `Capget`, `Capset` | no phase yet |
| `Reboot` | no phase yet; `boot-3` of the kernel plan adds the call |

Each moves into the posix block before the phase that needs it starts. The move
is a commit in the mlibc fork, not in this repository, and the phase cannot
build before it.

The precedent is termios. `2c9bd366` merged `termios-ioctls-fix` (`a726e6d9`,
`4f19ded2`, `bbce2de3`, `5e513879`), which took the termios tags and the
`winsize` definitions out of the Linux optionals. Without it, the port would
have inherited the Linux ioctl numbers as soon as it declared one terminal
tag.

A tag moves only when a phase needs it. Do not move a whole block, and do not
declare `linux` for a single call.

The other way past a block is to write the public function by hand in
`sysdeps/vex/`, against a header of the port. `generic/mount.cpp` does that for
`mount` and `umount`, and the port's own `sys/mount.h` declares them. A
function that the Linux headers declare, such as `umount2`, can go the same
way while the block stays off: no other header declares it then.

### Tags that no phase covers

These tags are declared in a block that is on and the kernel has the call, but
no phase lists them. Add a phase entry, or a line in "Out of scope".

| Tag | Block | Kernel call |
| --- | --- | --- |
| `FutexTid` | default | `SYS_GETTID` |
| `GetMaxPriority`, `GetMinPriority` | posix | `SYS_GETSCHEDRANGE`, which reports `min` and `max` |
| `Pause` | posix | `SYS_SIGSUSPEND` with the current mask |
| `PosixMadvise` | posix | `SYS_MADVISE` |

## Phase 1: threads

This phase unblocks every pthread feature. The design follows the managarm
port, which has the same kind of native thread call.

Files:

- `x86_64/thread_entry.S`: the first instruction of a new thread.
- `generic/thread.cpp`: the C part and the three tags.

Tasks:

1. `PrepareStack` allocates the stack with `VmMap` when `*stack` is null.
   Set `*guard_size` to 0 and `*stack_base` to the mapping. Push three
   values at the top, in this order: `tcb`, `user_arg`, `entry`. Set `*stack`
   to the new stack pointer.
2. `thread_entry.S` pops `rdi`, `rsi` and `rdx`, calls
   `__mlibc_enter_thread(entry, user_arg, tcb)`, and ends with `ud2`. Add the
   CFI directives that the managarm copy has.
3. `__mlibc_enter_thread` waits for `tcb->tid` with `FutexWait`, then calls
   `tcb->invokeThreadFunc(entry, user_arg)`, then `mlibc::thread_exit`.
   Do not call `TcbSet`: the kernel sets the FS base from the third argument
   of `SYS_THREAD_CREATE`.
4. `Clone(tcb, pid_out, stack)` calls
   `SYS_THREAD_CREATE(&thread_entry, stack, tcb, 0, 0)`. The kernel starts the
   thread at the trampoline, sets `rsp` to `stack` and the FS base to `tcb`.
   Return the tid in `pid_out`.
5. `ThreadExit` calls `SYS_THREAD_EXIT`.
6. Do not use `clear_tid`. mlibc signals a join with `tcb->didExit` and a
   futex wake in `thread_exit`. The parent writes `tcb->tid` after `Clone`
   returns and wakes it, as `thread_create` does today.

The kernel call checks the five arguments and returns the tid. A null
`clear_tid` is allowed, and the kernel skips the clear.

`FutexWait` and `FutexWake` already work and pass `FUTEX_SHARED`. Do not
change them.

Done when a program creates a thread, joins it, and passes a shared mutex
between two threads.

## Phase 2: signals

| Tag | Signature | Kernel call | Notes |
| --- | --- | --- | --- |
| `Sigaction` | `(int, const sigaction *, sigaction *)` | `SYS_SIGACTION` | Always set `SA_RESTORER`. The call fails with `EINVAL` without it |
| `Sigprocmask` | `(int, const sigset_t *, sigset_t *)` | `SYS_SIGPROCMASK` | |
| `Sigpending` | `(sigset_t *)` | `SYS_SIGPENDING` | |
| `Sigaltstack` | `(const stack_t *, stack_t *)` | `SYS_SIGALTSTACK` | `stack_t` is `__k_stack` |
| `Sigsuspend` | `(const sigset_t *)` | `SYS_SIGSUSPEND` | The mask is a pointer. The call returns `EINTR` |
| `Sigtimedwait` | `(const sigset_t *, siginfo_t *, const timespec *, int *)` | `SYS_SIGTIMEDWAIT` | The kernel writes the siginfo. A null timeout waits without a limit |
| `Sigqueue` | `(pid_t, int, const union sigval)` | `SYS_SIGQUEUE` | Pass the value in one register |

The restorer is the trap of this phase. The kernel builds the frame and puts
the restorer address on top of the user stack. A handler returns into that
address, and `rsp` then points at the `ucontext`. The restorer must not move
`rsp`. It does this:

```
mov $__k_SYS_SIGRETURN, %rax
syscall
ud2
```

Add it as `x86_64/sigreturn.S` and give the address to `sa_restorer` in
`Sigaction`. The kernel reads the `ucontext` from the stack pointer of the
call, so a single `syscall` instruction is enough.

Done when a program catches `SIGUSR1`, blocks it, takes it with
`sigtimedwait`, and returns from a handler with a modified mask.

## Phase 3: descriptors and files

| Tag | Signature | Kernel call | Notes |
| --- | --- | --- | --- |
| `Pread` | `(int, void *, size_t, off_t, ssize_t *)` | `SYS_PREAD` | |
| `Pwrite` | `(int, const void *, size_t, off_t, ssize_t *)` | `SYS_PWRITE` | |
| `Readv` | `(int, const iovec *, int, ssize_t *)` | `SYS_READV` | The kernel rejects more than `IOV_MAX` (1024) |
| `Writev` | `(int, const iovec *, int, ssize_t *)` | `SYS_WRITEV` | |
| `Pipe` | `(int *, int)` | `SYS_PIPE` | Takes `O_CLOEXEC` and `O_NONBLOCK` |
| `Flock` | `(int, int)` | `SYS_FLOCK` | `LOCK_SH`, `LOCK_EX`, `LOCK_UN`, `LOCK_NB` |
| `Fallocate` | `(int, off_t, size_t)` | `SYS_FALLOCATE` | Mode 0: the tag carries no mode, and `posix_fallocate` is the caller. The kernel also takes `FALLOC_FL_KEEP_SIZE`, which the tag cannot pass |
| `Mknodat` | `(int, const char *, int, int)` | `SYS_MKNODAT` | `dev` is an `int` in the tag and a 64 bit `dev_t` in the kernel. Convert it |
| `Mkfifoat` | `(int, const char *, mode_t)` | `SYS_MKNODAT` | Make it with `S_IFIFO` |
| `Chroot` | `(const char *)` | `SYS_CHROOT` | Needs `CAP_SYS_CHROOT` |
| `Syncfs` | `(int)` | `SYS_SYNCFS` | Linux block. Move the tag first, see "Options that gate a sysdep" |
| `Fadvise` | `(int, off_t, off_t, int)` | `SYS_MADVISE` or no-op | The kernel has no per-file advice. Return 0 for a known advice value |

The lock commands in `Fcntl` share the `__k_flock` copy with `Flock`:

- POSIX locks are 6 to 8 and belong to the process. They go when the process
  closes any descriptor of the file.
- OFD locks are 9 to 11 and belong to the open file. `l_pid` is 0 on input
  and -1 in a result.
- `flock` conflicts with `fcntl` locks.

Done when a program copies a file with `pread` and `pwrite`, writes a pipe,
and takes a conflicting lock.

## Phase 4: memory

| Tag | Signature | Kernel call | Notes |
| --- | --- | --- | --- |
| `VmProtect` | `(void *, size_t, int)` | `SYS_MPROTECT` | `prot` is the mlibc value |
| `Msync` | `(void *, size_t, int)` | `SYS_MSYNC` | |
| `Madvise` | `(void *, size_t, int)` | `SYS_MADVISE` | `MADV_NORMAL` to `MADV_DONTNEED` |
| `Mlock` | `(const void *, size_t)` | `SYS_MLOCK` | Counts bytes against `RLIMIT_MEMLOCK` |
| `Munlock` | `(const void *, size_t)` | `SYS_MUNLOCK` | |
| `Mlockall` | `(int)` | `SYS_MLOCKALL` | Needs `MCL_CURRENT` or `MCL_FUTURE` |
| `Munlockall` | `()` | `SYS_MUNLOCKALL` | |
| `VmRemap` | `(void *, size_t, size_t, void **)` | `SYS_MREMAP` | Always `MREMAP_MAYMOVE` |
| `VmReadahead` | `(void *, size_t)` | `SYS_MADVISE` with `MADV_WILLNEED` | The kernel ignores it today. This is honest |

Notes:

- `MAP_NORESERVE` does nothing without `CAP_VM_NORESERVE`. The kernel ignores
  it for other processes and does not fail.
- A private anonymous mapping takes its pages from the commit limit when it
  appears. `mmap` can fail with `ENOMEM` before the first touch.
- `mlock` does not pin frames, because the kernel has no swap. The call is
  honest for the current kernel.

Done when a program maps a file, writes it back with `msync`, and reads the
data through a second mapping.

## Phase 5: processes and wait

| Tag | Signature | Kernel call | Notes |
| --- | --- | --- | --- |
| `Waitid` | `(idtype_t, id_t, siginfo_t *, int)` | `SYS_WAITID` | The kernel takes a fifth argument, the `rusage` pointer. Pass 0 |
| `GetRusage` | `(int, rusage *)` | `SYS_GETRUSAGE` | `RUSAGE_SELF`, `RUSAGE_CHILDREN`, `RUSAGE_THREAD` |
| `GetRlimit` | `(int, rlimit *)` | `SYS_GETRLIMIT` | |
| `SetRlimit` | `(int, const rlimit *)` | `SYS_SETRLIMIT` | |
| `GetPriority` | `(int, id_t, int *)` | `SYS_GETPRIORITY` | The result is 1 to 40. Subtract the bias |
| `SetPriority` | `(int, id_t, int)` | `SYS_SETPRIORITY` | |
| `Nice` | `(int, int *)` | `SYS_SETPRIORITY` and `SYS_GETPRIORITY` | `PRIO_PROCESS` with the caller |
| `GetScheduler` | `(pid_t, int *)` | `SYS_GETSCHEDULER` | |
| `SetScheduler` | `(pid_t, int, const sched_param *)` | `SYS_SETSCHEDULER` | Only `SCHED_OTHER`, priority 0 |
| `GetSchedparam` | `(void *, int *, sched_param *)` | `SYS_GETSCHEDPARAM` | The tag takes a TCB, and the kernel call takes a process. Policy and priority belong to the process, so pass 0 for the caller and report `SCHED_OTHER` |
| `SetSchedparam` | `(void *, int, const sched_param *)` | `SYS_SETSCHEDPARAM` | Pass 0 for the process. Copy the priority into `__k_sched_param` |
| `GetParam` | `(pid_t, sched_param *)` | `SYS_GETSCHEDPARAM` | |
| `SetParam` | `(pid_t, const sched_param *)` | `SYS_SETSCHEDPARAM` | |
| `GetAffinity` | `(pid_t, size_t, cpu_set_t *)` | `SYS_GETAFFINITY` | `tid` is 0. `cpusetsize` is in bytes |
| `SetAffinity` | `(pid_t, size_t, const cpu_set_t *)` | `SYS_SETAFFINITY` | `tid` is 0, so the call sets every thread of the process |
| `GetThreadaffinity` | `(pid_t, size_t, cpu_set_t *)` | `SYS_GETAFFINITY` | `pid` is 0. The `tid` is the tag argument |
| `SetThreadaffinity` | `(pid_t, size_t, const cpu_set_t *)` | `SYS_SETAFFINITY` | `pid` is 0 |
| `Getcpu` | `(int *)` | `SYS_GETCPU` | Linux block. Move the tag first |
| `Umount2` | `(const char *, int)` | `SYS_UMOUNT` | Only the plain unmount exists. Linux block. Move the tag first |

Notes:

- The kernel's affinity size is in bytes and the call returns the kernel's
  own byte count, which can be smaller than `cpusetsize`. mlibc's
  `cpusetsize` is in bytes. No conversion is needed.
- A thread of another process needs its process id in the kernel call. The
  mlibc tags carry only the tid, so the sysdep passes 0 for the process.
  `pthread_setaffinity_np` takes a TCB of the own process, so this is enough.
- mlibc has no `Spawn` tag. `posix_spawn` runs on `Fork` and `Execve`. Using
  `SYS_SPAWN` needs a change in `options/posix/generic/spawn.cpp`, not a new
  sysdep. Leave it for later. The kernel keeps the number and gives the call a
  header of its own when a user appears.
- `Umount2` is not reachable as a tag until the block moves. The port already
  writes `mount()` and `umount()` by hand in `generic/mount.cpp`, against its
  own `sys/mount.h`. `umount2()` can go in the same place when a caller
  appears.

Done when a program runs a child with `posix_spawn`, waits for it, and reads
the child usage.

## Phase 6: poll

| Tag | Signature | Kernel call | Notes |
| --- | --- | --- | --- |
| `Poll` | `(pollfd *, nfds_t, int, int *)` | `SYS_PPOLL` | The tag timeout is in milliseconds. Convert it to a relative `timespec`. -1 means no timeout |
| `Ppoll` | `(pollfd *, nfds_t, const timespec *, const sigset_t *, int *)` | `SYS_PPOLL` | Pass the arguments as they are |
| `Pselect` | `(int, fd_set *, fd_set *, fd_set *, const timespec *, const sigset_t *, int *)` | `SYS_PPOLL` | Build the `pollfd` array from the sets, and pass the mask |

Notes:

- The kernel writes no remaining time.
- A negative descriptor is skipped. An invalid one gives `POLLNVAL`.
- `count` above `RLIMIT_NOFILE` gives `EINVAL`.
- A signal that ends the wait gives `EINTR`. `Pselect` must return `EINTR`,
  never a restart.

Done when a program waits for a pipe with a timeout, and a signal ends the
wait with `EINTR`.

## Phase 7: time, random and sysconf

| Tag | Signature | Kernel call | Notes |
| --- | --- | --- | --- |
| `ClockGetres` | `(int, time_t *, long *)` | `SYS_GETTIMERES` | A null pointer checks only that the clock exists |
| `ClockSet` | `(int, time_t, long)` | `SYS_SETTIME` | `CLOCK_REALTIME` only. Needs `CAP_SYS_TIME` |
| `GetItimer` | `(int, itimerval *)` | `SYS_SETITIMER` | A null new value only reads |
| `SetItimer` | `(int, const itimerval *, itimerval *)` | `SYS_SETITIMER` | `ITIMER_REAL`, `ITIMER_VIRTUAL`, `ITIMER_PROF` |
| `Times` | `(tms *, clock_t *)` | `SYS_GETRUSAGE` and `SYS_GETTIME` | Fill `tms` from the process usage, and the elapsed ticks from `CLOCK_MONOTONIC` |
| `GetEntropy` | `(void *, size_t)` | `SYS_GETRANDOM` | Flags 0. The call may wait for the first seed |
| `Sysinfo` | `(sysinfo *)` | `SYS_SYSINFO` | Linux block. Move the tag first. The kernel structure is not the Linux one. Convert it: `uptime` in seconds, `totalram` and `freeram` in bytes from the page counts, and zero the rest |
| `Sysconf` | `(int, long *)` | `SYS_SYSINFO`, `SYS_GETRLIMIT` and the auxv | Page size from `AT_PAGESZ`, ticks from `AT_CLKTCK`, CPUs and pages from `SYSINFO` |
| `GetHostname` | `(char *, size_t)` | `SYS_UNAME` | The kernel has no gethostname call. Copy `nodename` from uname |
| `SetHostname` | `(const char *, size_t)` | `SYS_SETHOSTNAME` | Needs `CAP_SYS_ADMIN` |

Notes:

- `CLOCK_BOOTTIME` returns `EINVAL`. Do not alias it to `CLOCK_MONOTONIC`.
- The CPU time clocks are in `SYS_GETTIME` and need no new tag.
- `getrandom` with no `GRND_INSECURE` may wait for the first seed. The boot
  reaches a seed before init runs.
- The kernel defines `__k_AT_CLKTCK`, but it never writes the pair: the auxv
  ends at `AT_SECURE` (`kernel/exec/elf64.c` lines 268 to 280). `Sysconf`
  cannot read the tick rate from it. Add the pair to the kernel, which is not a
  new call, or report the fixed 100. Prefer the pair.

Done when a program measures a real and a monotonic interval, reads 32 bytes
from `getrandom`, and prints the CPU count from `sysconf`.

## Phase 8: cleanup

1. Remove the stubs that the kernel now supports.
2. Search for `ENOSYS` in the sysdeps tree. Each hit is a parked kernel item
   or a bug in this plan.
3. Run the mlibc test suite on the Vex image. Record every failure here with
   the kernel item that explains it.

## ABI traps

| Trap | Rule |
| --- | --- |
| `waitpid` | The fourth argument is the `rusage` pointer |
| `waitid` | The fifth argument is the `rusage` pointer. mlibc passes 0 |
| `dup3` with equal numbers | `EINVAL`. `dup2` with equal numbers succeeds |
| `SYS_DUP` flags | 0 or `O_CLOEXEC`. Nothing else |
| `sigaction` | Always set `SA_RESTORER` |
| Restorer | It must not move `rsp` |
| `getpriority` | Returns 1 to 40. Subtract the bias |
| Affinity | The size is in bytes. The call returns the byte count |
| `ppoll` | A relative timeout, a mask pointer, no write-back |
| `SYS_SPAWN` | mlibc does not call it. `posix_spawn` uses fork and exec |
| `fcntl` locks | The kernel numbers are 6 to 11, not the Linux numbers |
| `O_TMPFILE` | `__k_O_TMPFILE` is 0x0400 |
| `MAP_NORESERVE` | Ignored without `CAP_VM_NORESERVE`, and not an error |
| `Sysinfo` | The kernel structure differs from the Linux one. Convert it |
| Signal frame | `(sig, siginfo *, ucontext *)` in `rdi`, `rsi`, `rdx` |
| Error returns | The kernel returns `-errno`. Convert at the wrapper |

## Kernel gaps that mlibc may hit

| Kernel item | Effect on mlibc |
| --- | --- |
| `proc-1` | A woken thread waits for the current slice. A two-thread test with different nice values may show it |
| `ipc-2` | A robust mutex needs `FUTEX_ROBUST`, which the kernel lacks |
| `mm-2` | `mlockall` applies nothing for `MCL_CURRENT` and `MCL_FUTURE` |
| `fs-1` | A device node outside devfs does not open |
| `sig-1` | `SA_NOCLDWAIT` leaves zombies |
| `boot-1` | The test init does not sync and unmount, so a test image needs the sync that the panic path does |

## Out of scope

The kernel has no call for these tags, and this plan does not add one:
sockets, `aio`, `epoll`, `inotify`, `timerfd`, `signalfd`, `eventfd`, SysV
IPC, `memfd_create`, `ptrace`, `process_vm_readv`, pidfd, pty and kernel
modules. They need a kernel subsystem of their own.

## Done criteria

- Every tag in `VexSysdepTags` has an implementation, or a comment that names
  the kernel item that blocks it.
- Every tag of a phase is in a block that is on. A tag of the `linux` block
  moves into the posix block before its phase starts.
- The ABI headers match `kernel/include/usr/vex/`. The `static_assert`s in
  Phase 0 hold.
- The mlibc test suite runs on the Vex image. A failure has a kernel item or
  a repair in this plan.
- No sysdep returns `ENOSYS` for a call that the kernel implements.

## Not verified

- The entry point and the options of the mlibc test suite in the bootstrap
  recipe.
- Whether `pthread_create` sets `tcb->tid` in every path. The code at
  `options/internal/generic/threads.cpp:132` does, and the join waits on
  `didExit`, which fits the design of Phase 1.
- Which callers of `Pselect` pass a signal mask.
- The layout of `struct sysinfo` on this mlibc version. Check it against
  `usr/vex/sysinfo.h` before the conversion.
- The `Fadvise` tag has no kernel call. A no-op may be enough, but check
  which caller uses it.
