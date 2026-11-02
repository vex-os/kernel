# tmpfs

tmpfs is the in-memory filesystem. It has no volume and no copy on a disk:
the pages of a file are the only copy. Userspace mounts it for a directory
that must not survive a reboot, and the kernel makes instances of it that no
path leads to.

## Discovery

A filesystem has no discovery step. `init_tmpfs` adds the `tmpfs` driver to
the registry of the VFS at the vfs stage, and a mount finds it by that name.
The driver is not marked internal, so userspace can mount it.

`vfs_create()` makes an instance with no device and no mount point, and
nothing can unmount it. [Shared memory](../architecture/ipc/shmem.md) uses one
such instance for the file behind a shared anonymous mapping.

## Attach

The mount takes no block volume, and a mount that names one fails with
`EINVAL`. The options come from the mount parameters:

| Option | Meaning |
| --- | --- |
| `size` | The most pages of file contents, with an IEC suffix such as `64m` |
| `nodes` | The most inodes |
| `mode` | The permission bits of the root directory, in octal |
| `uid` | The owner of the root directory |
| `gid` | The group of the root directory |

The root directory defaults to mode `01777` and to the owner and group 0. Any
process may create a file in it, and only the owner may remove one. Mounted
with no option, the filesystem has no limit of its own and the
[commit limit](../architecture/memory/commit.md) is the only bound.

A mount holds one mutex, one list of inodes, the two limits and two counters.

## Data path

Every node has a vnode and a `tmpfs_inode_t`. The inode holds the permission
bits, the owner, the link count and three timestamps. It also holds the size,
the target of a symbolic link and the entries of a directory. The type lives
in the vnode, and the inode number comes from a counter of the mount.

The data pages come from the file cache with `fcache_set_memory()`. That mode
makes the pages of the vnode the only copy. They are never dirty, and the
cache needs no `bmap` and no volume. A read of a hole gives zeroes and takes
no page, and reclaim never takes a page. Nothing writes the pages back,
because there is nowhere to write them.

A write holds the mutex of the mount. When it extends the file, it first
charges the pages of the new size to the limit of the mount and to the commit
limit. If the charge fails, the write fails whole. A short write then shrinks
the file back to the bytes that it kept. `truncate` and `fallocate` use the
same resize, and a shorter file gives its pages back.

A page fault runs `fcache_fault()`, which puts the page of the file into the
page map of the process. The pages were paid for when the size grew, so a
fault does not charge them again.

A directory is a list of entries, in the order that they were created. Each
entry carries a sequence number that grows and never repeats, and `getdents`
reports that number as the position. A reader that removes an entry in the
middle does not move the position of the entries after it. A lookup walks the
list and compares the names.

A symbolic link keeps its target in the inode. A target of at most
`SLAB_MAX_SIZE` bytes sits in a slab object, and a longer one takes a page.
A FIFO, a character device and a block device have an inode and no data.

An entry holds one reference to its vnode for as long as the name exists. An
`unlink` drops the name and the reference, and it removes the node from the
cache of the vnode when the link count reaches 0. The last reference, which an
open file or a mapping can still hold, frees the pages, the target and the
inode.

## Why it is done this way

- **The pages are the only copy.** A tmpfs has no disk to read from and no
  writeback to schedule. The memory mode of the file cache removes the `bmap`,
  the dirty list and the volume that other filesystems need.
- **The whole size is charged.** A hole costs a page in the commit limit,
  because the file can be written there at any time. This keeps a write from
  failing after it has already copied part of the data.
- **One mutex for the mount.** The filesystem is small and its operations are
  short. A lock per inode would add nothing but state.
- **Sequence numbers in the directories.** POSIX lets another thread remove an
  entry between two `getdents` calls, and the position of the reader must
  still name the next entry.
- **No discovery.** A tmpfs instance exists only because somebody mounted it,
  or because the kernel created one.

## Limits

- Every operation takes the one mutex, so two writes to different files also
  wait for each other.
- A directory lookup walks a list. A directory with many entries is slow.
- There are no extended attributes and no per-user quota. The `size` and
  `nodes` options are the whole limit.
- A page is never reclaimed or moved out. The file holds it until a truncate
  or an unlink.
- A sparse file still counts its holes in the commit limit.
- Nothing survives a reboot.

## Source

- [tmpfs.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/tmpfs/tmpfs.c#L93) — the mount, the options and the limits
- [tmpfs_vnode.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/tmpfs/tmpfs_vnode.c#L924) — the vnode operations
- [tmpfs_internal.h](https://github.com/vex-os/kernel/blob/main/kernel/fs/tmpfs/tmpfs_internal.h#L18) — the mount and the inode
- [fcache.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/fcache.h#L42) — the memory mode of the file cache
- [vnode.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/vnode.h) — the operations that the vnode offers
