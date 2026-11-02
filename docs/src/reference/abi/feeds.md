# Device feeds

A feed is a virtual node of devfs that gives records of kernel events. Vex has
two: `/dev/kplog` for the kernel log and `/dev/hwtree` for the device tree.
Each `read` gives only whole records. When the next record does not fit in the
buffer of the caller, the read fails with `EINVAL`. A program therefore reads
with a buffer that is large enough for the largest record.

Every record begins with `struct __k_feed_header`:

| Field | Meaning |
| --- | --- |
| `size` | The bytes of the whole record, header and padding included, a multiple of 8 |
| `type` | The kind of record, never zero |
| `flags` | Their meaning depends on `type` |
| `seq` | The sequence number of the record |
| `time_ns` | The moment of the record on `__k_CLOCK_MONOTONIC` |

The type `__k_FEED_TYPE_LOST` says that the feed dropped records before this
one, and `struct __k_feed_lost` gives the count. A type of a device is
`__k_FEED_TYPE_DEF(n)`, which is `0x0100 + n`.

## The kernel log

A `struct __k_kplog_record` adds the subsystem, the level, the length and the
text. The text is not terminated, and its field holds at most
`__k_KPLOG_TEXT_MAX` (512) bytes. The flag `__k_KPLOG_FLAG_TRUNCATED` says
that the message was longer than the record.

The levels run from `__k_KP_EMERG` (0) to `__k_KP_VERBOSE` (7). The subsystem
is one of `__k_KP_*`, and `__k_KP_USER` marks a write from user space. A write
to `/dev/kplog` needs `CAP_SYSLOG`.

The requests of the `'L'` group:

| Request | Argument | Meaning |
| --- | --- | --- |
| `__k_KPLOG_SEEK_FIRST` | None | Go to the oldest record in the ring |
| `__k_KPLOG_SEEK_END` | None | Go to the record to come |
| `__k_KPLOG_SEEK_SEQ` | `__k_uint64_t` | Go to that sequence number |
| `__k_KPLOG_GET_INFO` | `__k_kplog_info` | The first and the next sequence number |
| `__k_KPLOG_GET_CONSOLE_LEVEL` | `__k_int32_t` | The level that the console prints |
| `__k_KPLOG_SET_CONSOLE_LEVEL` | `__k_int32_t` | Set that level |

## The device tree

A `struct __k_hwtree_record` adds the id of the node, the id of its parent, its
state and a block of fields. The fields are zero-separated `key=value` pairs
in one array of at most `__k_HWTREE_FIELDS_MAX` (512) bytes. The flag
`__k_HWTREE_FLAG_TRUNCATED` says that some fields did not fit and were left
out whole.

A reader gets the tree as it was at open, between a snapshot begin record and
a snapshot end record, with parents before their children. After that come the
changes. After a lost record comes a new snapshot.

| Type | Meaning |
| --- | --- |
| `__k_FEED_TYPE_HWTREE_SNAPSHOT_BEGIN` | The beginning of a snapshot |
| `__k_FEED_TYPE_HWTREE_SNAPSHOT_END` | The end of a snapshot |
| `__k_FEED_TYPE_HWTREE_ADD` | A node appeared |
| `__k_FEED_TYPE_HWTREE_REMOVE` | A node left |
| `__k_FEED_TYPE_HWTREE_BIND` | A driver took the node |
| `__k_FEED_TYPE_HWTREE_UNBIND` | The driver let it go |
| `__k_FEED_TYPE_HWTREE_WAITING` | The best driver waits for a provider |
| `__k_FEED_TYPE_HWTREE_ATTACH_FAILED` | The attach returned an error |
| `__k_FEED_TYPE_HWTREE_INTERFACE` | An interface appeared on the node |
| `__k_FEED_TYPE_HWTREE_INTERFACE_REMOVE` | The interface left |

The state of a node is one of `__k_HWTREE_STATE_PRESENT`, `PROBING`,
`WAITING`, `BOUND`, `DETACHING` and `GONE`.

The requests of the `'H'` group change the tree, so they need a file opened
for writing, and that open needs `CAP_DEVICE`:

| Request | Argument | Meaning |
| --- | --- | --- |
| `__k_HWTREE_RESNAPSHOT` | None | Give the tree again from the beginning |
| `__k_HWTREE_RESCAN` | `__k_uint64_t` | Rescan the buses below that node |
| `__k_HWTREE_UNBIND` | `__k_uint64_t` | Unbind the driver of that node |
| `__k_HWTREE_EJECT` | `__k_uint64_t` | Eject the device of that node |

## Limits

- A record is at most the fixed part plus 512 bytes of text or fields, and its
  size is a multiple of 8.
- A read buffer that is too small for the next record fails with `EINVAL`. The
  kernel does not cut a record and does not skip one.
- The log is a ring. A reader that falls behind gets a lost record and misses
  the records in between.

## Source

- [feed.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/feed.h#L1) — the common header and the lost record
- [kplog.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/kplog.h#L1) — the log record and the `'L'` requests
- [hwtree.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/hwtree.h#L1) — the tree record and the `'H'` requests
