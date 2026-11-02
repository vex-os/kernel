# ABI

The ABI is the set of headers that a program outside the kernel compiles
against. They live in `kernel/include/usr/vex/` and a program includes them as
`<vex/...>`. They fix the numbers of the system calls, the errors, the ioctls
and the layout of every structure that a call passes.

## Where it lives

The headers under `kernel/include/usr/` are the whole userspace ABI. The
architecture adds its own, such as `arch/amd64/include/usr/arch/limits.h`, and
`<vex/types.h>` includes the one of the target. The test image compiles
against both directories, and a C library takes the same two.

A header here compiles as ANSI C89 or ISO C90. It uses `/* */` comments, no
`nullptr`, no `bool`, no `static_assert`, no `restrict`, no `inline` and no
designated initializers, and it declares every variable at the start of its
block. A C library can therefore read the headers with an old compiler.

The guard comes from the path: `usr/vex/errno.h` becomes `USR_VEX_ERRNO_H`,
and the `#endif` repeats the name. The name never starts with `_`.

Only `usr/vex/types.h` uses `<stdint.h>`, and only when the compiler is C23 or
C++. In C89 the header picks its types from the widths in `<limits.h>`.

## Names

Every name in the ABI starts with `__k_`. That prefix keeps the POSIX name
free for the C library:

| ABI | C library | Kernel |
| --- | --- | --- |
| `__k_EINVAL` | `EINVAL` | `EINVAL` |
| `__k_O_RDONLY` | `O_RDONLY` | `O_RDONLY` |
| `__k_VINTR` | `VINTR` | `VINTR` |
| `struct __k_stat` | `struct stat` | `stat_t` |

Most names reach the C library this way. A few constants, such as the
`RLIMIT_*` numbers, the library writes as the same literal. It also adds a few
flags of its own, such as the `O_*` bits that the kernel ignores.

The system call numbers are the exception: there is no public `SYS_*` name.
The C library in the bootstrap tree, mlibc, includes `<vex/syscall.h>` inside
its own system call layer and passes `__k_SYS_*` to the kernel. A program calls
`read`, and not a number.

The kernel does not include the ABI headers directly. A header under
`kernel/include/vex/` includes the one under `usr/`, defines the plain names
for kernel code, and gives each structure its `_t` typedef.

## Types

`usr/vex/types.h` defines every integer the ABI uses:

| Type | Width | Use |
| --- | --- | --- |
| `__k_int8_t` … `__k_int64_t` | 8, 16, 32, 64 | Signed fields |
| `__k_uint8_t` … `__k_uint64_t` | 8, 16, 32, 64 | Unsigned fields |
| `__k_intptr_t`, `__k_uintptr_t` | The pointer width | An address in an integer |
| `__k_ssize_t`, `__k_size_t` | 64 | A count or a result with an error |
| `__k_id_t`, `__k_pid_t`, `__k_uid_t`, `__k_gid_t` | 32 | Ids |
| `__k_off_t` | 64 | A file offset |
| `__k_mode_t` | 32 | A mode, a flag or an `int` argument |
| `__k_dev_t`, `__k_ino_t` | 64 | A device and an inode |
| `__k_time_t`, `__k_clock_t` | 64 | Seconds and ticks |

A structure in the ABI uses these types for every field, and never `int`,
`long` or a plain pointer where an integer holds an address. The layout then
does not depend on the widths that the compiler gives to its plain types.

A field with the address of user memory is a `__k_uintptr_t` or a pointer. A
field with a size in memory is a `__k_size_t`. A structure keeps the order
that its header shows, and its fields are naturally aligned.

`struct __k_stat` is the largest common example. It holds the device, the
inode, the mode and the link count. It also holds the owner and group, the file
size, three `__k_timespec` values and the block counts. Its mode constants are
the ext2 ones, `__k_S_IFREG` and the rest, so a filesystem passes the value
through without a translation.

`struct __k_dirent` is the record of `getdents`: inode, the position of the
next record, the length of this record, the type and the name. The record ends
after the name, padded to 8 bytes, and `d_reclen` is the step to the next one.

## Errors

A system call returns the negated error number in its result register. The C
library turns that into `-1` and stores the positive number in `errno`. The
numbers are Vex numbers and not the Linux ones. A library defines each kernel
error from the header, and not from a number that it chose. It may add numbers
above them for errors that the kernel never returns. [Errors](errors.md) holds
the whole list.

## ioctls

An ioctl request is one 32-bit number with a direction, the size of its
argument, a group and a command. [ioctls](ioctls.md) gives the layout and the
groups. Each group page lists the requests of its device class:

- [Serial port](serial.md) covers the `'S'` group, the line settings and the
  binding to a consumer.
- [Terminal](terminal.md) covers the `'T'` group and the line discipline.
- [Framebuffer](framebuffer.md) covers the `'F'` group.
- [Device feeds](feeds.md) covers the `'L'` and `'H'` groups and the record
  format that `/dev/kplog` and `/dev/hwtree` use.

## The headers

| Area | Headers |
| --- | --- |
| Types and limits | `types.h`, `limits.h`, `errno.h` |
| Calls | `syscall.h` |
| Files | `fcntl.h`, `stat.h`, `statvfs.h`, `dirent.h`, `uio.h`, `mount.h`, `xattr.h` |
| Processes | `signal.h`, `wait.h`, `sched.h`, `resource.h`, `time.h`, `futex.h`, `spawn.h` |
| Memory | `mman.h` |
| Devices | `ioctl.h`, `termios.h`, `serial.h`, `fb.h`, `dev.h`, `feed.h`, `kplog.h`, `hwtree.h` |
| Kernel facts | `sysinfo.h`, `utsname.h`, `auxv.h`, `random.h` |
| Security | `capability.h` |

The values of a header that an outside standard fixes, such as the `S_IF*`
bits of `stat.h` and the `RLIMIT_*` numbers of `resource.h`, keep the values
of that standard. A value that Vex invents starts at a free bit or a free
number and never moves.

## Changing the ABI

There is no version field in these headers. A program that a compiler built
against an old copy of them keeps calling with the old numbers. A value that a
program can see must not change. A new option gets a new bit, a new error gets
the next number, and a new structure is a new name. A change to a field of an
existing structure changes the ABI, and the ioctl request of the call carries
its size.

## Source

- [types.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/types.h#L1) — the integer types of the ABI
- [stat.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/stat.h#L1) — `struct __k_stat` and the mode bits
- [dirent.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/dirent.h#L1) — `struct __k_dirent`
- [syscall.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/syscall.h#L1) — the system call numbers
