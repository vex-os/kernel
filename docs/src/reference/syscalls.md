# Syscalls

This page lists every system call: its number, the arguments that the call
reads, and the kernel function that runs it. The mechanism is in
[System calls](../architecture/syscalls.md).

The numbers come from `kernel/include/usr/vex/syscall.h`. Each subsystem owns a
block of 32 numbers that starts at a multiple of `0x20`. The table gives the
calls of each block in number order and leaves out the numbers that no call
uses. A call with such a number fails with `ENOSYS`. Every number in the
header has a handler.

## Conventions

- The arguments are in the order of the registers, one for each register. A
  call reads at most six.
- `user_addr_t` is a pointer into the address space of the caller. The handler
  copies through it. It is never a kernel pointer.
- A descriptor is an `int`. When the `_fd` macro registered the handler, the
  handler gets the resolved file instead, and the table shows the descriptor
  that the caller passes.
- A call with a `dirfd` also takes the path as a user pointer. The kernel
  copies the path before it uses it.
- The return value is an `intptr_t`. A negative value is an errno number that
  is negated, and every other value is a result.
- The link on a name goes to the handler.

## Processes and scheduling

| Number | Call | Arguments |
| --- | --- | --- |
| `0x020` | [`SYS_EXIT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L716) | `int code` |
| `0x021` | [`SYS_FORK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L730) | `none` |
| `0x022` | [`SYS_EXEC`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L779) | `user_addr_t path, user_addr_t argv, user_addr_t envp` |
| `0x023` | [`SYS_WAITPID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L987) | `pid_t pid, user_addr_t wstatus, uintptr_t options, user_addr_t usage` |
| `0x024` | [`SYS_GETPID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L1110) | `none` |
| `0x025` | [`SYS_GETPPID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L1124) | `none` |
| `0x026` | [`SYS_SETSID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L1138) | `none` |
| `0x027` | [`SYS_SETPGID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L1152) | `pid_t pid, pid_t pgid` |
| `0x028` | [`SYS_GETPGID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L1183) | `pid_t pid` |
| `0x029` | [`SYS_GETSID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L1206) | `pid_t pid` |
| `0x02B` | [`SYS_GETPRIORITY`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L190) | `int which, pid_t who` |
| `0x02C` | [`SYS_SETPRIORITY`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L214) | `int which, pid_t who, intptr_t value` |
| `0x02D` | [`SYS_GETSCHEDULER`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L248) | `pid_t pid` |
| `0x02E` | [`SYS_SETSCHEDULER`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L314) | `pid_t pid, uintptr_t policy, user_addr_t param` |
| `0x02F` | [`SYS_GETSCHEDPARAM`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L328) | `pid_t pid, user_addr_t param` |
| `0x030` | [`SYS_SETSCHEDPARAM`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L321) | `pid_t pid, user_addr_t param` |
| `0x031` | [`SYS_GETSCHEDRANGE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L360) | `uintptr_t policy, user_addr_t range` |
| `0x032` | [`SYS_GETSCHEDINTERVAL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L378) | `pid_t pid, user_addr_t interval` |
| `0x033` | [`SYS_GETAFFINITY`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L506) | `pid_t pid, pid_t tid, size_t size, user_addr_t mask` |
| `0x034` | [`SYS_SETAFFINITY`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L550) | `pid_t pid, pid_t tid, size_t size, user_addr_t mask` |
| `0x035` | [`SYS_WAITID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L1045) | `unsigned idtype, pid_t id, user_addr_t info, uintptr_t options, user_addr_t usage` |
| `0x036` | [`SYS_GETCPU`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L618) | `none` |
| `0x037` | [`SYS_SPAWN`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L809) | `user_addr_t address` |

## Threads

| Number | Call | Arguments |
| --- | --- | --- |
| `0x040` | [`SYS_THREAD_CREATE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_thread.c#L21) | `user_addr_t entry, user_addr_t stack_top, user_addr_t tcb, uintptr_t argument, user_addr_t clear_tid` |
| `0x041` | [`SYS_THREAD_EXIT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_thread.c#L59) | `none` |
| `0x042` | [`SYS_GETTID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_thread.c#L89) | `none` |
| `0x043` | [`SYS_TCB_SET`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_thread.c#L102) | `user_addr_t tcb` |
| `0x044` | [`SYS_YIELD`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_thread.c#L122) | `none` |
| `0x045` | [`SYS_FUTEX_WAIT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_futex.c#L21) | `user_addr_t address, uint32_t value, user_addr_t timeout, uintptr_t flags` |
| `0x046` | [`SYS_FUTEX_WAKE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_futex.c#L58) | `user_addr_t address, size_t count, uintptr_t flags` |

## Signals

| Number | Call | Arguments |
| --- | --- | --- |
| `0x060` | [`SYS_KILL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L121) | `pid_t pid, int sig` |
| `0x061` | [`SYS_TGKILL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L165) | `pid_t pid, pid_t tid, int sig` |
| `0x062` | [`SYS_SIGACTION`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L264) | `int sig, user_addr_t act, user_addr_t oact` |
| `0x063` | [`SYS_SIGPROCMASK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L314) | `int how, user_addr_t set, user_addr_t oset` |
| `0x064` | [`SYS_SIGPENDING`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L354) | `user_addr_t set` |
| `0x065` | [`SYS_SIGRETURN`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L374) | `none` |
| `0x066` | [`SYS_SIGALTSTACK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L559) | `user_addr_t ss, user_addr_t old` |
| `0x067` | [`SYS_SIGSUSPEND`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L400) | `user_addr_t set` |
| `0x068` | [`SYS_SIGTIMEDWAIT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L503) | `user_addr_t set, user_addr_t info, user_addr_t timeout` |
| `0x069` | [`SYS_SIGQUEUE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_signal.c#L218) | `pid_t pid, int sig, uintptr_t value` |
| `0x06A` | [`SYS_SETITIMER`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_time.c#L323) | `int which, user_addr_t value, user_addr_t old` |

## Credentials

| Number | Call | Arguments |
| --- | --- | --- |
| `0x080` | [`SYS_GETRESUID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L31) | `user_addr_t ruid, user_addr_t euid, user_addr_t suid` |
| `0x081` | [`SYS_SETRESUID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L47) | `uid_t ruid, uid_t euid, uid_t suid` |
| `0x082` | [`SYS_GETRESGID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L79) | `user_addr_t rgid, user_addr_t egid, user_addr_t sgid` |
| `0x083` | [`SYS_SETRESGID`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L95) | `gid_t rgid, gid_t egid, gid_t sgid` |
| `0x084` | [`SYS_GETGROUPS`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L127) | `size_t size, user_addr_t list` |
| `0x085` | [`SYS_SETGROUPS`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L156) | `size_t size, user_addr_t list` |
| `0x086` | [`SYS_GETUMASK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L204) | `none` |
| `0x087` | [`SYS_SETUMASK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L218) | `mode_t mask` |
| `0x088` | [`SYS_CAPGET`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L232) | `user_addr_t caps` |
| `0x089` | [`SYS_CAPSET`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L261) | `user_addr_t caps` |

## Memory

| Number | Call | Arguments |
| --- | --- | --- |
| `0x0A0` | [`SYS_MMAP`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L144) | `user_addr_t addr, uintptr_t length, uintptr_t prot, uintptr_t flags, int fd, uintptr_t offset` |
| `0x0A1` | [`SYS_MUNMAP`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L236) | `user_addr_t addr, uintptr_t length` |
| `0x0A2` | [`SYS_MPROTECT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L257) | `user_addr_t addr, uintptr_t length, uintptr_t prot` |
| `0x0A3` | [`SYS_MSYNC`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L278) | `user_addr_t addr, uintptr_t length, uintptr_t flags` |
| `0x0A4` | [`SYS_MADVISE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L318) | `user_addr_t addr, uintptr_t length, uintptr_t advice` |
| `0x0A5` | [`SYS_MLOCK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L362) | `user_addr_t addr, uintptr_t length` |
| `0x0A6` | [`SYS_MUNLOCK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L397) | `user_addr_t addr, uintptr_t length` |
| `0x0A7` | [`SYS_MLOCKALL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L473) | `uintptr_t flags` |
| `0x0A8` | [`SYS_MUNLOCKALL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L491) | `none` |
| `0x0A9` | [`SYS_MREMAP`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L430) | `user_addr_t addr, uintptr_t old_size, uintptr_t new_size, uintptr_t flags` |

## Files

| Number | Call | Arguments |
| --- | --- | --- |
| `0x0C0` | [`SYS_OPENAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L817) | `int dirfd, const char* path, uintptr_t flags, mode_t mode` |
| `0x0C1` | [`SYS_CLOSE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L865) | `int fd` |
| `0x0C2` | [`SYS_READ`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L349) | `int fd, user_addr_t buffer, size_t count` |
| `0x0C3` | [`SYS_WRITE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L359) | `int fd, user_addr_t buffer, size_t count` |
| `0x0C4` | [`SYS_SEEK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L449) | `int fd, off_t offset, int origin` |
| `0x0C5` | [`SYS_IOCTL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L494) | `int fd, uint32_t request, uintptr_t argument` |
| `0x0C6` | [`SYS_DUP`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L879) | `int old_fd, int new_fd, uintptr_t flags` |
| `0x0C9` | [`SYS_FSTAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L474) | `int fd, user_addr_t stat` |
| `0x0CA` | [`SYS_FTRUNCATE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L456) | `int fd, off_t length` |
| `0x0CB` | [`SYS_FSYNC`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L501) | `int fd` |
| `0x0CC` | [`SYS_FCNTL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L766) | `int fd, unsigned command, uintptr_t argument` |
| `0x0CD` | [`SYS_PREAD`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L371) | `int fd, user_addr_t buffer, size_t count, off_t offset` |
| `0x0CE` | [`SYS_PWRITE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L381) | `int fd, user_addr_t buffer, size_t count, off_t offset` |
| `0x0CF` | [`SYS_READV`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L391) | `int fd, user_addr_t iov, size_t count` |
| `0x0D0` | [`SYS_WRITEV`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L405) | `int fd, user_addr_t iov, size_t count` |
| `0x0D1` | [`SYS_PREADV`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L421) | `int fd, user_addr_t iov, size_t count, off_t offset` |
| `0x0D2` | [`SYS_PWRITEV`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L435) | `int fd, user_addr_t iov, size_t count, off_t offset` |
| `0x0D3` | [`SYS_PIPE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L913) | `user_addr_t fds, uintptr_t flags` |
| `0x0D4` | [`SYS_PPOLL`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_poll.c#L282) | `user_addr_t fds, size_t count, user_addr_t timeout, user_addr_t mask` |
| `0x0D5` | [`SYS_FLOCK`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L787) | `int fd, uintptr_t operation` |
| `0x0D6` | [`SYS_FALLOCATE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_file.c#L465) | `int fd, uintptr_t mode, off_t offset, off_t length` |

## Paths

| Number | Call | Arguments |
| --- | --- | --- |
| `0x0E0` | [`SYS_FSTATAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L160) | `int dirfd, const char* path, user_addr_t stat, unsigned flags` |
| `0x0E1` | [`SYS_FCHMODAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L252) | `int dirfd, const char* path, mode_t mode, unsigned flags` |
| `0x0E2` | [`SYS_FCHOWNAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L281) | `int dirfd, const char* path, uid_t uid, gid_t gid, unsigned flags` |
| `0x0E3` | [`SYS_UTIMENSAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L310) | `int dirfd, const char* path, user_addr_t times, unsigned flags` |
| `0x0E4` | [`SYS_MKDIRAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L406) | `int dirfd, const char* path, mode_t mode` |
| `0x0E5` | [`SYS_UNLINKAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L461) | `int dirfd, const char* path, unsigned flags` |
| `0x0E6` | [`SYS_RENAMEAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L508) | `int old_dirfd, user_addr_t old_path, int new_dirfd, user_addr_t new_path` |
| `0x0E7` | [`SYS_LINKAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L560) | `int old_dirfd, user_addr_t old_path, int new_dirfd, user_addr_t new_path, unsigned flags` |
| `0x0E8` | [`SYS_SYMLINKAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L618) | `user_addr_t target, int dirfd, user_addr_t path` |
| `0x0E9` | [`SYS_READLINKAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L665) | `int dirfd, const char* path, user_addr_t buffer, size_t size` |
| `0x0EA` | [`SYS_GETDENTS`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L120) | `int fd, user_addr_t buffer, size_t size` |
| `0x0EB` | [`SYS_GETCWD`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L709) | `user_addr_t buffer, size_t size` |
| `0x0EC` | [`SYS_CHDIR`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L752) | `user_addr_t path` |
| `0x0ED` | [`SYS_FCHDIR`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L867) | `int fd` |
| `0x0EE` | [`SYS_FACCESSAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L200) | `int dirfd, const char* path, unsigned mode, unsigned flags` |
| `0x0EF` | [`SYS_CHROOT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L808) | `user_addr_t path` |
| `0x0F0` | [`SYS_MKNODAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L436) | `int dirfd, const char* path, mode_t mode, dev_t rdev` |

## Extended attributes and mounts

| Number | Call | Arguments |
| --- | --- | --- |
| `0x100` | [`SYS_GETXATTRAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_xattr.c#L31) | `int dirfd, const char* path, user_addr_t name, user_addr_t value, size_t size, unsigned flags` |
| `0x101` | [`SYS_SETXATTRAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_xattr.c#L91) | `int dirfd, const char* path, user_addr_t name, user_addr_t value, size_t size, unsigned flags` |
| `0x102` | [`SYS_LISTXATTRAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_xattr.c#L147) | `int dirfd, const char* path, user_addr_t list, size_t size, unsigned flags` |
| `0x103` | [`SYS_REMOVEXATTRAT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_xattr.c#L200) | `int dirfd, const char* path, user_addr_t name, unsigned flags` |
| `0x104` | [`SYS_MOUNT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_vfs.c#L34) | `user_addr_t source, user_addr_t target, user_addr_t type, uintptr_t flags, user_addr_t params` |
| `0x105` | [`SYS_UMOUNT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_vfs.c#L147) | `user_addr_t path` |
| `0x106` | [`SYS_SYNC`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_vfs.c#L181) | `none` |
| `0x107` | [`SYS_SYNCFS`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_vfs.c#L188) | `int fd` |
| `0x108` | [`SYS_STATVFS`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_vfs.c#L213) | `int fd, user_addr_t buffer` |

## System

| Number | Call | Arguments |
| --- | --- | --- |
| `0x120` | [`SYS_UNAME`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_uname.c#L20) | `user_addr_t buffer` |
| `0x121` | [`SYS_GETTIME`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_time.c#L63) | `uintptr_t clock, user_addr_t time` |
| `0x122` | [`SYS_SETTIME`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_time.c#L107) | `uintptr_t clock, user_addr_t time` |
| `0x123` | [`SYS_SETHOSTNAME`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_uname.c#L40) | `user_addr_t name, size_t length` |
| `0x124` | [`SYS_NANOSLEEP`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_time.c#L253) | `uintptr_t clock_id, uintptr_t flags, user_addr_t request, user_addr_t remaining` |
| `0x125` | [`SYS_GETTIMERES`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_time.c#L138) | `uintptr_t clock, user_addr_t resolution` |
| `0x126` | [`SYS_SYSINFO`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_uname.c#L68) | `user_addr_t buffer` |
| `0x127` | [`SYS_GETRLIMIT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_rlimit.c#L14) | `uintptr_t resource, user_addr_t out` |
| `0x128` | [`SYS_SETRLIMIT`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_rlimit.c#L39) | `uintptr_t resource, user_addr_t in` |
| `0x129` | [`SYS_GETRUSAGE`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_rlimit.c#L74) | `intptr_t who, user_addr_t out` |
| `0x12A` | [`SYS_GETRANDOM`](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_random.c#L16) | `user_addr_t buffer, size_t size, uintptr_t flags` |
