# devfs

devfs is the filesystem of device nodes. It has one directory, its root, and
one node for each block or char interface that the kernel publishes. A node
appears when the interface appears, and it goes when the interface goes.

## Discovery

devfs does not search for hardware. It subscribes to the two event buses of
the device model, one for block interfaces and one for char interfaces. Each
subscription replays every interface that already exists. A devfs that is
mounted after the disks are up therefore shows them at once.

A block node appears on `BLOCK_EVENT_MEDIA_CONNECTED` or
`BLOCK_EVENT_PARTITION_ADDED`, and it goes on `BLOCK_EVENT_MEDIA_DISCONNECTED`
or `BLOCK_EVENT_PARTITION_REMOVED`. An interface with `BLOCK_FLAG_HIDDEN` gets
no node. A char node appears on `CHAR_EVENT_INTERFACE_ADDED` and goes on
`CHAR_EVENT_INTERFACE_REMOVED`. `CHAR_EVENT_MEMORY_CHANGED` revokes the
mappings of the node and keeps it.

The name of the node is the name of the interface, in the form that
[Device names](../reference/device-names.md) describes, such as `ada0n1p1`,
`fbd0` or `serial1`. The kernel services that show their own state, such as
`/dev/kplog` and `/dev/hwtree`, register a char interface, so devfs makes their
nodes like any other.

## Attach

A mount allocates the state of the filesystem, which is one lock, one list of
nodes and one count, and makes the root directory. The root has mode `0555`,
and the mount reads neither a block volume nor an option string.

The mount then subscribes to the two buses. When a subscription fails, the
mount removes what it made so far and fails. An unmount unsubscribes, removes
every node from the cache of vnodes, and drops the reference of each node and
of the root.

The kernel does not mount devfs. An init process mounts it on `/dev`, and the
test image mounts it there before it runs the first test.

## Data path

A lookup walks the list of nodes and compares the names. The inode number of a
node is its vnode pointer, so it is unique while the node lives and costs no
allocation. An iteration gives the dots first, then the nodes in the order of
the list. The list is unsorted, and each insert puts the new node at the head.

A char node keeps a reference to its interface. Every operation of the vnode
forwards to the char interface. Open and close make the per-file context.
`read`, `write`, `seek`, `ioctl`, `poll` and `mmap` call the matching op.
`stat` reports `S_IFCHR` with mode `0660` and the `rdev` of the interface.

A block node keeps the interface in `vnode->volume` and gives the device as a
file of bytes. A read or a write maps the byte offset to a sector and a byte
inside it. A run that starts on a sector and covers whole sectors goes to
`block_read()` or `block_write()` in one call. A partial first or last sector
goes through one bounce buffer of the node, which a mutex protects. A write
reaches the device at once, with no page cache to hold it. `stat` reports the
size as the sector count times the sector size.

A removal takes the node out of the list and out of the cache, and it drops
the reference of the filesystem. An open file or a mapping still holds a
reference, so the vnode and its context live until the last one goes.

## Why it is done this way

- **The bus replays.** The mount does not need a second path to find the
  devices that already exist. A device that arrives later uses the same handler
  as one that was there first.
- **The interface owns the work.** devfs forwards every operation, so a driver
  has one implementation, in the char or block interface, and not a second one
  in the filesystem.
- **The vnode pointer is the inode number.** A devfs node has no number of its
  own to allocate or to free.
- **A separate handler per interface kind.** The two paths share the list and
  the cache, and nothing else, so a char device does not carry block fields.

## Limits

- devfs has one directory. There is no `/dev/disk/by-id`, no per-bus directory
  and no symbolic link.
- A lookup and an iteration walk the list. The position of an iteration is an
  index in that list. A node that is added or removed between two `getdents`
  calls can then move what the reader sees next.
- The permissions are fixed: `0555` for the root, `0660` for a char or block
  node. There is no request that changes them, and no create or mknod op, so
  userspace cannot add a node.
- A block node offers `read`, `write` and `stat`. It has no `ioctl`, no
  `poll`, no `mmap` and no `seek`.
- A name is unique only because the interfaces keep their names unique. A
  second insert with the same name adds a second node, and a lookup finds the
  newest one.

## Source

- [devfs.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/devfs/devfs.c#L49) — the mount, the list and the insert
- [devfs_dir.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/devfs/devfs_dir.c#L11) — the root directory
- [devfs_char.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/devfs/devfs_char.c#L130) — the char nodes and the event handler
- [devfs_block.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/devfs/devfs_block.c#L246) — the block nodes and the bounce buffer
- [devfs_internal.h](https://github.com/vex-os/kernel/blob/main/kernel/fs/devfs/devfs_internal.h#L12) — the node and the state
