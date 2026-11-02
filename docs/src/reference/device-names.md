# Device names

Every block and char interface gets a name when it registers. Userspace opens
that name under `/dev`. The interface also keeps a 64-bit `dev_t`, and the
kernel keeps the name and the number in step.

A name is stable text. For example, `ada0n1p2` names a partition of an ATA
disk. A `dev_t` is the same device as one integer, which `stat` reports as
`st_rdev`.

## The name

A name has four parts:

```
<prefix><unit>n<index>p<partition>
   ada     0     n1      p2
```

- The prefix is the device class, in letters.
- The unit is the number that the class gives the device.
- The index is a connector or a lane below the device.
- The partition is a slice of the block device.

The letters `n` and `p` are literal, and each one introduces the part after it.
The parts nest. A partition needs an index, and an index needs a unit. The
format rejects a name that breaks this rule. A name with no part for a level
stops at the last part that it has.

| Name | Prefix | Unit | Index | Partition |
| --- | --- | --- | --- | --- |
| `kplog` | `kplog` | none | none | none |
| `fbd0` | `fbd` | 0 | none | none |
| `serial1` | `serial` | 1 | none | none |
| `ada0` | `ada` | 0 | none | none |
| `ada0n1` | `ada` | 0 | 1 | none |
| `ada0n1p2` | `ada` | 0 | 1 | 2 |
| `tty1337` | `tty` | 1337 | none | none |

A unit and an index use 12 bits each. A partition uses 8 bits. A part that
does not fit fails with `-ERANGE`. A name of 64 characters or more fails with
`-ENAMETOOLONG`, because the buffer holds 63 characters.

## Prefixes

Each prefix is one global `dev_prefix_t` in
[dev_name.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/dev_name.c).
The same file lists them in `dev_prefixes[]`.

| Prefix | Form | Class |
| --- | --- | --- |
| `ada` | `ada<unit>` | ATA disk |
| `acd` | `acd<unit>` | ATAPI drive |
| `nvd` | `nvd<unit>` | NVMe namespace, no driver yet |
| `vbd` | `vbd<unit>` | virtio block, no driver yet |
| `tty` | `tty<unit>` | Terminal on a serial port |
| `pty` | `pty<unit>` | Pseudo terminal, no user yet |
| `serial` | `serial<unit>` | Serial port |
| `serialtest` | `serialtest` | Far end of the test serial port |
| `fbd` | `fbd<unit>` | Framebuffer device |
| `kplog` | `kplog` | Kernel log |
| `hwtree` | `hwtree` | Device tree |
| `random` | `random` | Random source |
| `urandom` | `urandom` | Second random source, same behavior as `random` |
| `anon` | `anon<unit>` | Anonymous filesystem, no `/dev` node |

A new prefix needs three things:

- an entry in the header,
- a global in `dev_name.c`,
- a line in `dev_prefixes[]`.

There is no runtime registry.

## The device number

A `dev_t` holds the class in the top half and the parts in the low half:

```
 63              32 31         20 19          8 7      0
+----------------+------------+-------------+--------+
| prefix hash    | unit       | index       | part   |
+----------------+------------+-------------+--------+
```

| Field | Bits | Range |
| --- | --- | --- |
| Prefix | 63:32 | CRC-32/ISO-HDLC of the prefix text |
| Unit | 31:20 | 0 to 4094 |
| Index | 19:8 | 0 to 4094 |
| Partition | 7:0 | 0 to 254 |

`DEV_SET` packs the fields and `DEV_GET_*` reads them. `DEV_GET_MAJOR` is the
prefix hash and `DEV_GET_MINOR` is the low 32 bits. The macros are in
[usr/vex/dev.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/dev.h),
because a program reads them through `st_rdev`.

The kernel hashes every prefix once, in `init_dev_prefixes`. A panic stops the
boot when two prefixes have the same hash.

## Taking a number

Three calls give a name to an interface. All of them run under one spinlock.

`dev_unit_alloc` takes the next free number of a prefix. It takes the number
that a `unit=` option gives the device. Without one, it takes the lowest number
that no unit has. It also skips a number that a `unit=` option keeps for
another device. A device without a node passes `nullptr` and gets the lowest
free number.

`dev_unit_claim` takes one number that the caller chooses. The hardware, or a
binding option, decides it. It fails with `-EEXIST` when another unit has the
number. It fails with `-ERANGE` when a `dev_t` cannot hold the number.

`dev_prefix_claim` gives the whole prefix to one device, which then has no
unit. It fails when another device holds the prefix or any unit below it. No
unit can appear afterwards, until the owner calls `dev_prefix_release`.

`dev_unit_free` and `dev_prefix_release` release the name. The device that
holds the name must be gone first.

| Caller | Call | Name |
| --- | --- | --- |
| AHCI, PATA | `dev_unit_alloc` for the controller or channel. The index is the port or drive plus one | `ada0n1` |
| Partition table | root name with a partition | `ada0n1p2` |
| Serial port | `dev_unit_alloc` or `dev_unit_claim` | `serial1` |
| tty on a serial port | `dev_unit_alloc`, or the index of `serial=` | `tty1337` |
| Framebuffer | `dev_unit_alloc` | `fbd0` |
| `kplog`, `hwtree`, `random`, `urandom`, `serialtest` | `dev_prefix_claim` | `kplog` |
| Anonymous filesystem | `dev_unit_alloc` without a node | `anon0` |

The kernel warns when another unit already has the pinned number. The device
then takes the lowest free number.

## Pinning a name

A probe can run in a different order on each boot. The kernel therefore lets
the command line pin a name to a node:

```
unit=serial1,acpi:PCI0/pci:0000:00:1f.0/isa:3f8
```

The path is the one that `/dev/hwtree` shows for the node. It holds the device
names from the top-level device down, joined by `/`. A device name is at most
32 characters and the path at most 256.

The prefix of a `unit=` option is the run of letters before the first digit.
The kernel reads every option twice. `init_pinned_units` runs at the bus stage
and warns about a malformed option. `dev_unit_alloc` reads the options again,
at each registration.

An option keeps its number even for an interface that never appears. A second
device that asks for that number takes the lowest free number instead.

## The `/dev` namespace

devfs listens to the block and char events. It makes one node per interface,
with the interface name. It removes the node when its interface is removed.

`root=` is a block name, not a path. `mount_root` finds the interface with
`block_interface_find` and passes the interface to `vfs_mount`. The names
therefore exist before devfs mounts.

## Why it is done this way

- **A name does not depend on the probe order.** A test or an fstab entry needs
  the same name on each boot. `unit=` makes that name, and the test image pins
  its serial ports for this reason.
- **The unit is per class.** The prefix says what the device is and the unit
  says which one it is. A partition nests below an index, so `ada0n1p2` names
  both the disk and the slice.
- **The class lives in the number.** The kernel has no major and minor
  registry. A `dev_t` carries the hash of the prefix, so a new class needs no
  number allocation. `st_rdev` is enough to tell two devices apart.
- **A class of one claims its prefix.** A device such as `/dev/kplog` has no
  need for a number. The claim stops a later unit from taking the name.

## Limits

- A prefix has 4095 unit values, from 0 to 4094. The value 4095 is the
  sentinel `DEV_NO_UNIT`.
- A device has at most 254 partitions, numbered from 1.
- One spinlock covers every prefix. An allocation on one class waits for an
  allocation on another.
- The prefix table is code. A new class needs an edit of `dev_name.c` and a
  rebuild.
- A name has one owner. There are no aliases, and no second name for a
  partition.
- A wrong `unit=` path is not an error. The device takes the lowest free
  number, and the pinned number stays unused.

## Source

| File | Contents |
| --- | --- |
| [dev_name.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/dev_name.c) | The prefixes, the unit list, the allocation, the format |
| [dev_name.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/dev_name.h) | `dev_prefix_t`, `dev_unit_t`, `dev_name_t` and their calls |
| [usr/vex/dev.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/dev.h) | The bit layout of `dev_t` |
| [block_interface.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/block_interface.c) | The root and partition names of a block device |
| [char_interface.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/char_interface.c) | The name of a char device |
| [tty_serial.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/tty_serial.c) | The number that `serial=` gives a tty |
| [devfs_char.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/devfs/devfs_char.c) | The node under `/dev` and `st_rdev` |
