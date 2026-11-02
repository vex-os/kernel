# Shared memory

A shared anonymous mapping, `MAP_SHARED | MAP_ANONYMOUS`, is a mapping of a
file that no name leads to. The kernel makes the file in an internal
[tmpfs](../../filesystems/tmpfs.md), unlinks it at once, and maps it as a
shared file. Both processes of a fork then map the same file and see the same
pages.

POSIX shared memory is not a kernel object. Userspace mounts a tmpfs at a
fixed path, such as `/run/shmem`, and `shm_open` creates a file there. The C
library hardcodes that path. The mount needs `CAP_MOUNT`, so the init process
or a service manager makes it. The kernel serves the file with the same tmpfs
and `mmap` code as any other file.

## The file

`shmem_file_create()` makes the file. It takes a name from a counter, creates
a vnode in the internal tmpfs, and unlinks the name before it returns. Nothing
else can reach the file, so the name only serves the moment of creation, and
the number in it is never reused.

The call then sets the size of the file, and the tmpfs charges the whole size
to the [commit limit](../memory/commit.md). A file of 4 MiB takes 1024 pages
of 4 KiB at this point. The pages themselves arrive later, on the faults of
the mapping. The tmpfs of this file sets no page limit of its own, so the
commit limit is the whole bound.

## The mapping

`map_shared_anonymous()` maps the file with `backing.shared` set. The mapping
takes its own reference to the file, and the system call closes the file at
once. The mapping is the only thing that keeps the file and its pages alive,
and the last unmap takes the last reference and frees them.

The shared flag also changes how fork treats the mapping. A private anonymous
mapping is copied on write, so the child starts with its own pages. A shared
mapping of a file stays shared: both processes fault on the same vnode and get
the same page.

`MAP_NORESERVE` has no effect here. The whole size is taken from the commit
limit when the mapping appears, and the reserve of
[`CAP_VM_NORESERVE`](../../reference/capabilities.md#the-bits) changes nothing
for this kind of mapping.

## Why it is done this way

- **A file, not a new object.** The VFS already shares a file-backed region
  across fork, handles the faults and frees the pages on the last unmap. A
  second memory object with its own lifetime rules would repeat that work.
- **The anonymous mapping has no name.** It exists for a fork and nothing
  else. Named shared memory is a file on a mounted tmpfs. The path, the
  permissions and the removal rule belong to the filesystem, and not to a new
  kernel object.
- **The size is charged at once.** A shared mapping must not fail on a fault
  after another process already mapped it, so the whole range is reserved at
  `mmap`.
- **The file uses the kernel credentials.** It is made for the kernel and
  reached only through the mapping, so no path check applies to it.

## Limits

- The kernel has no `shm_open(3)`. A C library provides it on a tmpfs that
  userspace mounted, at a path that both agree on.
- A shared anonymous mapping has no name, so only the children of a fork
  reach it.
- There is no System V shared memory, no `memfd_create(2)` and no sealing.
- `msync` has nothing to flush for a tmpfs file.

## Source

- [shmem.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/shmem.c#L30) — the unnamed file and the tmpfs
- [shmem.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/shmem.h) — `shmem_file_create`
- [sys_mman.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L119) — the shared anonymous mapping
- [vms.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/vms.c#L729) — the reference that a region takes
- [tmpfs.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/tmpfs/tmpfs.c#L221) — the charge of the pages
