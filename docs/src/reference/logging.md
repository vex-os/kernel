# Logging

The kernel writes one message with `pr_*`. The call appends a record to a ring
buffer, and every console copies the new records to its device. Userspace reads
the same records from `/dev/kplog`.

The ring holds the last `CONFIG_KPRINTF_BUFSIZE` bytes. A console that appears
late can therefore replay the boot. A crash leaves the recent messages for a
reader.

## Levels

A level says how important a message is. The value is smaller for a more
important message.

| Value | Macro | Meaning |
| --- | --- | --- |
| 0 | `pr_emerg` | The system is unusable |
| 1 | `pr_alert` | Action is necessary at once, usually unrecoverable |
| 2 | `pr_critical` | Critical condition, probably unrecoverable |
| 3 | `pr_error` | Error condition, probably recoverable |
| 4 | `pr_warning` | Warning, definitely recoverable |
| 5 | `pr_notice` | Normal but important message |
| 6 | `pr_inform` | Informational message |
| 7 | `pr_verbose` | Debug message |

`pr_emerg` and the other macros pass a subsystem and a level to `kprintf`. A
newline at the end of the text does not enter the record.

The variable `loglevel` holds the level of the least important message that a
console writes. A console writes a record when `record.level <= loglevel`. The
default is `KP_INFORM`, so the kernel hides `pr_verbose` messages.

The `loglevel=` kernel option sets the variable. The option takes `verbose`,
`inform`, `notice`, `warning`, `error`, `critical` or `alert`, in any letter
case. It does not take `emerg`.

## Subsystems

A subsystem says which part of the kernel wrote the message. The build passes
one value to each archive: `kernel/meson.build` maps a top directory to a
`KP_*` value with `-DKP_SUBSYSTEM=`.

| Value | Name | Top directory |
| --- | --- | --- |
| 0x00 | `KP_CORE` | `core`, `fbcons`, `init`, `libk` |
| 0x01 | `KP_ARCH` | `arch/amd64` |
| 0x02 | `KP_MM` | `mm` |
| 0x03 | `KP_PROC` | `proc` |
| 0x04 | `KP_IRQ` | `irq` |
| 0x05 | `KP_TIME` | `time` |
| 0x06 | `KP_FIRMWARE` | `firmware` |
| 0x07 | `KP_DEVICE` | `device` |
| 0x08 | `KP_BUS` | `bus` |
| 0x09 | `KP_DEV` | `dev` |
| 0x0A | `KP_DRIVERS` | `drivers` |
| 0x0B | `KP_FS` | `fs` |
| 0x0C | `KP_EXEC` | `exec` |
| 0x0D | `KP_CRYPTO` | `crypto` |
| 0x0E | `KP_IPC` | `ipc` |
| 0x0F | `KP_RANDOM` | `random` |
| 0xFF | `KP_USER` | A write to `/dev/kplog` |

A file that needs another subsystem defines `KP_SUBSYSTEM` before its
includes. Without that define the header picks `KP_CORE`.

## The ring buffer

The ring is one static array. Each record starts with a `kp_record_t` header
and holds the text after it. The record and its text take a whole number of
8-byte units.

```
 ring_tail           ring_head
 |                    |
 v                    v
+-------+-------+-------+-------+-----------+
| rec 0 | rec 1 | rec 2 | rec 3 |   free    |
+-------+-------+-------+-------+-----------+
```

| Field | Contents |
| --- | --- |
| `seq` | The record number, from 0, one more for each record |
| `time_ns` | Nanoseconds since boot |
| `size` | Text size, without a newline or a NUL |
| `subsystem` | One `KP_*` value |
| `level` | One `pr_*` value |
| `flags` | `KPLOG_FLAG_TRUNCATED` when the formatter cut the text |

A write holds the ring lock with interrupts off, formats the text, and appends
one record. A text longer than 512 bytes loses its tail and gets the truncated
flag. The kernel drops the oldest records when the ring is full.

The tail and the head are byte positions that only grow. A record that would
cross the end of the array moves to the beginning. The kernel writes a padding
marker when the space left can hold one. The head moves only after the whole
record is in the array. A panic therefore never reads half a record.

The append triggers the `kprintf_appended` event and flushes every console.

## Consoles

A console is one `console_t` with its own read cursor. A flush gives each
console the records that its cursor has not passed. The console core serializes
the calls.

| Console | Name | Flags |
| --- | --- | --- |
| Serial port | `serial<N>` | `CONSF_RECALL` |
| Framebuffer | `fbd0` | `CONSF_RECALL` |
| Bochs debug port | `bxcons` | `CONSF_RECALL`, `CONSF_LOGLEVEL` |

A console with `CONSF_RECALL` starts at the oldest record and replays the
history. A console without it starts at the newest record. `bxcons` sets its
own level to `KP_VERBOSE`, so it prints every record that the other consoles
hide.

The output line holds a timestamp, the text, and CR LF:

```
[   12.3456] kernel message
```

The timestamp is the time since boot in seconds, then four digits of the
fraction, so its resolution is 100 µs.

The first `open` of a device releases its console. The driver then uses the
port for its own data. The kernel keeps no claim on the device.

A panic takes every released device back with `takeover`, because the message
must reach the terminal. A clean shutdown will take the devices back in the
same way. The kernel has no clean shutdown yet, so a panic is the only
takeover.

A console that falls behind writes the count of records that it missed:

```
** 32 messages dropped **
```

## `/dev/kplog`

`/dev/kplog` is a char device. A read gives whole feed records. Each record
starts with a `feed_header`: the size, the type, the flags, the sequence number
and the timestamp. The subsystem, the level, the text size and the text follow.
A read that has no new record waits for the next append. `O_NONBLOCK` makes it
return `-EAGAIN`.

A record that the ring dropped arrives as a lost record: the feed type is
`FEED_TYPE_LOST` and the header holds the count. A read that cannot fit the
next record fails with `-EINVAL`.

A write appends one record at level `KP_NOTICE`, or at the level of a leading
`<N>` prefix, as syslog writes it. The kernel rejects a write with an embedded
line break or a level more severe than `KP_ERROR`. An open for write needs
`CAP_SYSLOG`.

| ioctl | Action |
| --- | --- |
| `KPLOG_SEEK_FIRST` | Move the cursor to the oldest record |
| `KPLOG_SEEK_END` | Move the cursor past the newest record |
| `KPLOG_SEEK_SEQ` | Move the cursor to a sequence number |
| `KPLOG_GET_INFO` | Read the first and the next sequence number |
| `KPLOG_GET_CONSOLE_LEVEL` | Read `loglevel` |
| `KPLOG_SET_CONSOLE_LEVEL` | Write `loglevel`, with a write-capable descriptor |

An open starts at the oldest record, so a program sees the boot. The ioctl
names say "console level", but the value is the global `loglevel` of the
consoles.

## Panic and assertions

`panic(fmt, ...)` records the file and the line at `KP_EMERG`, then the
formatted message. It stops the other harts with an NMI and waits about one
second for them to stop. It sets `panic_in_progress`, takes every released
console back, writes the message, and spins for good.

While `panic_in_progress` is true, the ring and the console core skip their
locks. The panic can thus log while another hart holds a lock that will never
be released.

A second panic on any hart stops that hart and does not write a message.

`kassert(expr)` checks an expression with `CONFIG_KASSERT`, which defaults to
yes. A false expression panics with the expression, the file, the line and the
function. Without the option, the kernel still evaluates the expression and
ignores the result.

## Why it is done this way

- **Records, not lines.** A console can appear after the boot or go away. The
  ring keeps the records, and each console reads them at its own pace.
- **Every level enters the ring.** A console filters when it writes, so
  `bxcons` and `/dev/kplog` see the verbose records that the serial console
  hides.
- **Growing positions, not indexes.** A reader keeps a byte position and a
  sequence number. A dropped record then becomes a count, not a wrong record.
- **A panic writes without locks.** A hart can panic with a lock held. The
  ring and the console core allow this after `panic_in_progress`, and the head
  shows only whole records.
- **A console borrows its device.** The kernel does not own an output device as
  Linux does. The driver owns the device, and the first open takes it from the
  console without a question. This simple rule has no state machine for a
  device that the kernel and a program share.
- **Userspace draws after the boot.** The kernel keeps one small framebuffer
  console. The build fixes its font and its character size (`CONFIG_FBCONS`,
  `FBCONS_FONT`). A program that opens `fbd0` gets the screen and must draw on
  it, as `kmscon` does on Linux. A future X server would own the screen in the
  same way. The kernel asks for the device back only when it must report a
  panic or a shutdown.

## Limits

- The kernel cannot take a device back from a running program. A panic, or a
  future clean shutdown, is the only takeover.
- The ring holds 64 KiB by default. `CONFIG_KPRINTF_BUFSIZE` sets another
  size, from 2 KiB to 1 MiB, in a multiple of 8.
- One record holds 512 bytes of text. A longer text loses its tail.
- A console that does not set `CONSF_LOGLEVEL` shares the global `loglevel`.
- Userspace cannot write a record more severe than `KP_ERROR`.
- The command line cannot set `loglevel` to `emerg`.
- The timestamp has a resolution of 100 µs.

## Source

| File | Contents |
| --- | --- |
| [kprintf.c](https://github.com/vex-os/kernel/blob/main/kernel/core/kprintf.c) | The ring, the append, the readers, `loglevel` |
| [kprintf.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/kprintf.h) | `pr_*`, `kp_record_t`, the cursor calls |
| [console.c](https://github.com/vex-os/kernel/blob/main/kernel/core/console.c) | The console list, the filter, the flush, the dropped count |
| [console.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/console.h) | `console_t`, `console_ops_t`, the `CONSF_*` flags |
| [kplog.c](https://github.com/vex-os/kernel/blob/main/kernel/core/kplog.c) | `/dev/kplog`, its feed, its write and its ioctls |
| [usr/vex/kplog.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/kplog.h) | The level and subsystem values, the record, the ioctl numbers |
| [usr/vex/feed.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/feed.h) | The feed header and the lost record |
| [panic.c](https://github.com/vex-os/kernel/blob/main/kernel/core/panic.c) | The hart stop, the emergency output, `panic_in_progress` |
| [assert.c](https://github.com/vex-os/kernel/blob/main/kernel/core/assert.c) | The message of a failed assertion |
