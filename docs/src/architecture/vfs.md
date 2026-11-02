# VFS

The VFS turns a path into an object. It gives every filesystem one interface. The rest of the kernel then reads and writes a file without the name of the filesystem that holds it.

The design follows the vnode interface of Kleiman: *Vnodes: An Architecture for Multiple File System Types in Sun UNIX* ([PDF](https://github.com/vex-os/kernel/blob/main/docs/papers/kleiman86vnodes.pdf), 1986). A name and an inode are separate. A vnode stands for one inode. The VFS keeps no name cache. Its cache maps the inode number of a filesystem to the vnode of that inode.

## The objects

| Object | Type | Stands for |
| --- | --- | --- |
| Mounted filesystem | `vfs_t` (`mount.h:46`) | One filesystem instance, at one mount point |
| Vnode | `vnode_t` (`vnode.h:188`) | One inode of one filesystem |
| Open file | `file_t` (`file.h:20`) | One open file description |
| Cache entry | `vcache_entry_t` (`vcache.h:11`) | The map from a pair of filesystem and inode number to a vnode |

A `vfs_t` holds the root vnode, the ops table of the driver, the block volume, the device number, and the flags `readonly` and `unmounting`. It also counts the vnodes that lost their last name but still have references.

A `vnode_t` holds its type, its filesystem, its inode number and the ops table. It holds the parent of a directory, the filesystem mounted over it, the page cache of the inode, and the list of file locks. `vnode_alloc` makes an uncached vnode. The filesystem fills it in `lookup`, `create`, `mknod` or `symlink`.

A `file_t` holds the vnode, the access flags, the offset and a mode word. The `ctxdata` field belongs to the vnode and its open op. `file_dupe` shares the same `file_t`, so two descriptors share one offset.

![The objects of the VFS](vfs-objects.svg)

The interface itself is `vnode_ops_t` (`vnode.h:64`). A filesystem implements the operations that it supports. An operation set to `nullptr` fails in a default way that the VFS chooses. The table has no per-operation version number, so a new op needs a default in the VFS.

## The cache

A vnode starts with a reference count of one. The caller that got the vnode owns that reference. The last `vnode_decref` does not free the vnode at once. It calls `vcache_keep` (`vnode.c:691`):

- The VFS destroys a vnode that has no cache entry at once.
- A vnode that has an entry goes to the tail of the unused list.
- The VFS destroys the oldest entries past `CONFIG_VCACHE_MAX_UNUSED` (1024 by default).

The cache keeps a vnode without references, so a second lookup of the same path costs no disk access. `vcache_get` takes an entry back from the unused list and gives the caller a new reference.

The cache key is the pair of `fs` and `ino`, not the name (`vcache.c:21`). An inode takes one vnode in the whole system. Two names of one inode, such as a hard link, share the same vnode.

A pinned entry belongs to its owner, which holds a reference until it calls `vcache_remove` (`vcache.h:19`). Devfs uses this for every device node (`devfs.c:124`). The pin fails with `-EEXIST` when another vnode already holds the key.

A file that loses its last name while it is open becomes an orphan. `unlink` marks the vnode `orphaned` and adds one to `fs->orphans` (`vnode.c:310`). The vnode lives until its last reference goes. The count keeps the filesystem from unmounting, because an orphan has no cache entry for the reference check to see.

`vnode_destroy` (`vnode.c:706`) drops the page cache, the file locks and the parent reference. It calls `ops->release` last, which frees the storage of the inode. A filesystem therefore frees its inode data in `release`, not in `unlink`.

## Lookup and crossing

A lookup starts with a directory vnode and one name:

```
vnode_lookup(dir, name)
  |
  |-- vfs_cross_mount(dir) -> the root of a mounted filesystem, if any
  |
  |-- dir->ops->lookup(name) -> ino
  |
  |-- vcache_get(fs, ino) ------------ hit --> return the cached vnode
  |
  `-- vnode_alloc(fs, ino)
        |
        |-- fs->ops->fill_node(node)
        `-- vcache_get_or_insert(node)
```

The cache can already hold the node, because another thread looked it up in the meantime. `vcache_get_or_insert` then returns the cached vnode, and the caller destroys its new vnode (`vnode.c:113`).

A mount point changes the answer of every operation. `vfs_cross_mount` (`mount.c:650`) returns the root of the mounted filesystem in place of the mount point directory. Every operation that takes a vnode from a path calls it at the start. The vnode from a path lookup can be a mount point, and the operation that uses it crosses on its own.

A vnode that is not a directory never crosses. A mount point is always a directory.

`vfs_cross_mount` fails with `-EBUSY` while a filesystem unmounts. The operation then fails before it reaches the filesystem.

## Paths

`path_resolve_at` walks one component at a time (`path.c:170`). Each component:

1. Must not be longer than `NAME_MAX`. The VFS returns `-ENOTDIR` for a longer one.
2. Needs `X_OK` on the current directory.
3. Goes through `vnode_lookup`.
4. Follows a symbolic link, unless it is the last component and `flags` contains `PATH_NOFOLLOW`.

The walk skips a dot component. A dot-dot component goes to `parent_of`, which climbs the mount stack: the root of a mounted filesystem stands for the directory that it covers (`path.c:113`). The walk stops at the root of the calling process. It never leaves that tree.

The walk reads a symbolic link into one page. A relative target continues from the directory that holds the link. An absolute target continues from the root of the process. The depth stops at 8 links, and the walk fails with `-ELOOP` (`path.c:11`).

A trailing separator means that the path continues, as POSIX says. `link/` therefore follows the link even with `PATH_NOFOLLOW`, because the directory behind it must be a directory.

The root of the walk is per process. `process.c` sets `path_set_root_source` to the root of the current process (`process.c:1068`). `chroot` moves that root. A thread with no process root starts at the root of the VFS.

Two other entry points share the walk:

- `path_resolve_parent_at` resolves the directory that holds a new name and returns the name. `file_open` uses it for `O_CREAT`.
- `path_build` makes a path from a vnode. A vnode has no name, so it climbs to the root and iterates each parent directory to find the entry with the right inode number (`path.c:376`). A directory outside the root of the process fails with `-ENOENT`.

## Mount and unmount

A driver declares itself with a `vfs_driver_t` and calls `vfs_driver_add` in a `vfs` stage initcall. The call needs a name, a mount op and an unmount op. A second driver with the same name fails with `-EEXIST`.

`vfs_mount` takes a path and a type. A type of `nullptr` asks each driver to probe the device: the first `probe` that returns 0 wins (`mount.c:53`). The VFS itself reads `ro` and `readonly` from the parameters and sets `MNT_RDONLY` from the flags.

`vfs_mount_at` does the part that needs a vnode. The mount point must be a directory. A second mount on the same directory fails with `-EBUSY`. On success the vnode holds the mounted filesystem in `mounted_fs`, and the filesystem holds the vnode in `mountpoint`. The VFS holds one reference to the mount point, and the driver holds one to its root. The VFS also adds the filesystem to its list of mounts.

`kmain` mounts the root with a `nullptr` mount point (`kmain.c:39`). That filesystem becomes the root of the VFS, and its root vnode lives in `vfs_root`.

`vfs_create` makes a filesystem that no path leads to (`mount.h:73`). It has no device and no mount point, it is not in the list of mounts, and nothing can unmount it. Pipefs and the internal tmpfs of shared memory use it. A driver marks itself `internal` to keep `vfs_mount` from mounting it by name.

A busy filesystem refuses to unmount. `vfs_unmount_at` first sets `unmounting`, so new operations fail. It syncs the page cache, then checks two counts (`mount.c:470`):

- `fs->orphans` must be zero.
- The sum of the reference counts of the vnodes must not exceed the references that the VFS knows about. Those are the references of the root and of the owners of pinned entries, plus one per cached directory with a parent.

The second check is exact. A leaked reference to a vnode fails the unmount with `-EBUSY`, and the leak stays visible.

Unmount then syncs the filesystem, removes the entry, clears `mounted_fs`, evicts every vnode of the filesystem, and calls `ops->unmount`. The driver drops its own state and the reference to its root.

## Open files

`file_open` resolves the path. If the path does not exist and the flags have `O_CREAT`, it resolves the parent, checks `W_OK | X_OK`, and calls `vnode_create`. A lost race with another creator retries the lookup.

A create with `O_EXCL`, and any open with `O_NOFOLLOW`, resolves the last component with `PATH_NOFOLLOW`, so a symbolic link there counts as an existing name.

`file_open_node` then applies the checks that need the vnode (`file.c:89`):

- `O_DIRECTORY` needs a directory.
- A write to a directory fails with `-EISDIR`.
- A write to a regular file on a read-only filesystem fails with `-EROFS`. A device node stays writable, because its filesystem is not its device.
- `O_TRUNC` removes set-id bits and file capabilities first, then sets the size to zero.

The file layer decides if a file is seekable by asking `vnode_seek(..., SEEK_CUR)`. A file that answers `-ESPIPE` is a stream, such as a FIFO or a terminal. Every other answer makes the file seekable (`file.c:136`).

`file_read` and `file_write` take a pointer to the offset. A normal call passes `&file->offset`. `pread` and `pwrite` pass a private copy, so they do not move the shared offset. The file layer ignores `O_APPEND`: the caller picks the position.

A write to a regular file stops at `RLIMIT_FSIZE`. A write that starts at the limit fails with `-EFBIG` and sends `SIGXFSZ` to the calling thread. A file that is longer than the limit can still shrink.

The last `file_close` releases the file locks of the open file, calls `vnode_close`, drops the vnode reference and frees the `file_t`.

Two kinds of lock live on the vnode: a POSIX record lock, which belongs to a process, and an open-file lock, which belongs to a `file_t` (`file_lock.h:14`). The VFS keeps the ranges in a list on the vnode. A process loses its locks on a file when any descriptor of that file closes (`process.c:210`). The conflict test, the wait and the deadlock check are in [file_lock.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/file_lock.c).

## The page cache

Every vnode has an `fcache_t` (`vnode.h:203`). It holds the pages of the inode by page index, and it is the only cache of file data. A filesystem passes its block map to `fcache_read`, `fcache_write` and `fcache_fault`. The cache calls back for the sectors that it needs.

A page that a caller dirties goes to one global write-back queue. A thread writes the queue back at `CONFIG_FCACHE_WRITEBACK_MSEC` intervals. A write that finds more than `CONFIG_FCACHE_DIRTY_PERCENT` of memory dirty writes its own file back before it returns. `fcache_sync` writes back one filesystem, and unmount calls it.

A tmpfs vnode sets the cache to memory mode (`fcache_set_memory`): the pages are the only copy, they are never dirty, and a hole reads as zeroes.

The cache lives inside the vnode, so the vnode's lifetime sets its lifetime. `vnode_destroy` deinits the cache, and the deinit asserts that the vnode is not on the write-back queue.

## Invariants and traps

- **The cache key is `(fs, ino)`.** A filesystem must give one inode number to one file. A filesystem whose inode number follows the location of the entry calls `vcache_rekey` after a rename (`vnode.c:423`).
- **The last-name rule.** When the last name of an inode goes, the filesystem calls `vcache_remove` under its own lock, before the entry can be reused (`vnode.h:85`). Otherwise a new file at that location gets the vnode of the old one. The VFS cannot do this for the filesystem, because only the filesystem knows which name was the last.
- **The VFS holds no filesystem lock.** Each filesystem serializes its own operations with one mutex (`ext2_internal.h:386`, `fat_internal.h:173`, `echfs_internal.h:79`, `tmpfs_internal.h:19`). The VFS locks are narrow: `mounts_lock` for the mount list and the crossing, `cache_lock` for the cache, `drivers_lock` for the driver list. No filesystem call runs under one of them.
- **The per-vnode `lock` is not the file lock.** It guards the parent link and the capability snapshot. It does not guard the ops.
- **One mutex covers every rename in the kernel.** The mutex (`vnode.c:48`) also covers the cache steps of the rename, so they cannot interleave with another rename.
- **Crossing happens at the operation, not at the lookup.** A path can end on a mount point. The vnode is the mount point, and the next operation crosses it.
- **A dot-dot never leaves the root of the process.** The check is `current == root`, not "the parent is null" (`path.c:231`).
- **A missing op has a default.** `lookup`, `create`, `unlink`, `rename`, `read`, `write` and `stat` fail with `-ENOSYS`. `mknod`, `link`, `symlink` and `set_attr` fail with `-EPERM`. `fallocate` and the xattr ops fail with `-EOPNOTSUPP`. `mmap` succeeds.
- **The VFS owns the parts that every filesystem would repeat.** It applies the read-only check, the new-name check and the rename checks. It also applies the access time policy, the capability removal on write, and the namespace split of xattrs. A filesystem stores xattrs and does not check their permissions.
- **Access time** follows `CONFIG_ATIME_*`. The VFS sets it on a successful read and on the start of a directory iteration, through `set_attr` (`vnode.c:479`). A read-only filesystem and a filesystem without `set_attr` never get one.
- **A directory holds a reference to its parent.** The chain ends at the root, which has no parent. This reference is why the unmount check counts expected references. The check does not test for zero.
- **`vnode_fault`, `vnode_mmap`, `vnode_resolve_page` and `vnode_fallocate` do not cross a mount.** They run on the vnode of an open file. A mount point is a directory, so a file vnode never needs the crossing there.

## Limits

- The VFS keeps no name cache. Every lookup of a component asks the filesystem for the inode number, even when the cache holds the vnode. A name cache would need invalidation from every filesystem.
- One mutex covers all renames in the kernel. Two renames on unrelated filesystems wait for each other.
- The unused list of the cache is global. A filesystem that fills it evicts the unused vnodes of the other filesystems.
- A device node on a filesystem other than devfs does not open. `device_node_open` returns `-ENXIO`, because the kernel has no table from a device number to an interface (`vnode.c:215`).
- There are no mount namespaces. The kernel has one tree of mounts. A process root is the only per-process view of it.
- There is no lazy unmount. A busy filesystem stays mounted until its last user goes.

## Source

| File | Contents |
| --- | --- |
| [vnode.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/vnode.c) | The operation wrappers, the crossing, the checks, the times, the xattrs |
| [vcache.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/vcache.c) | The keyed cache, the unused list, the eviction |
| [mount.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/mount.c) | The driver list, mount, unmount, the root, `vfs_create` |
| [path.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/path.c) | The walk, the symbolic links, the dot-dot rule, `path_build` |
| [file.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/file.c) | Open, close, read, write, seek, the size limit |
| [fcache.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/fcache.c) | The page cache, the write-back queue, the fault path |
| [vnode.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/vnode.h) | `vnode_t`, `vnode_ops_t` and the contracts of the ops |
| [vcache.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/vcache.h) | `vcache_entry_t` and the cache calls |
| [mount.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/mount.h) | `vfs_t`, `vfs_ops_t`, the mount calls |
| [file.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/file.h) | `file_t` and the file calls |
| [fcache.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/fcache.h) | The page cache interface |
