# Capabilities

A capability is one bit of a 64-bit mask. The kernel tests the effective set
of the credentials of the thread at each privileged operation. The bits of the
Linux capabilities keep their Linux numbers, so a mask moves between the two
systems without a change of meaning. The Vex-only bits start at 48.

The check is `cred_capable()`. It reads one bit of `cap_effective`, and
nothing else.

## The bits

| Bit | Name | What it allows |
| --- | --- | --- |
| 0 | `CAP_CHOWN` | Change the owner or the group of a file |
| 1 | `CAP_DAC_OVERRIDE` | Bypass the read, write and execute permission checks |
| 3 | `CAP_FOWNER` | Act as the owner: chmod, timestamps, and removal from a sticky directory |
| 4 | `CAP_FSETID` | Keep the setuid and setgid bits that a change would clear |
| 5 | `CAP_KILL` | Signal a process of another user |
| 6 | `CAP_SETGID` | Change any group id, and set the supplementary groups |
| 7 | `CAP_SETUID` | Change any user id |
| 14 | `CAP_IPC_LOCK` | Lock memory beyond `RLIMIT_MEMLOCK` |
| 18 | `CAP_SYS_CHROOT` | Change the root directory |
| 21 | `CAP_SYS_ADMIN` | Read and write `trusted.*` extended attributes, write the other `security.*` ones, set the host name, and force a session on a terminal |
| 23 | `CAP_SYS_NICE` | Set the nice value of a process of another user, and lower its own past the floor |
| 24 | `CAP_SYS_RESOURCE` | Raise a hard resource limit |
| 25 | `CAP_SYS_TIME` | Step the realtime clock |
| 26 | `CAP_SYS_TTY_CONFIG` | Change the line settings of a serial port, and bind or unbind its terminal |
| 27 | `CAP_MKNOD` | Make a device node |
| 31 | `CAP_SETFCAP` | Set or remove the `security.capability` extended attribute |
| 34 | `CAP_SYSLOG` | Write to `/dev/kplog` |
| 48 | `CAP_MOUNT` | Mount and unmount a filesystem |
| 49 | `CAP_DEVICE` | Write to `/dev/hwtree`, which rescans, unbinds and ejects devices |
| 50 | `CAP_VM_NORESERVE` | Map memory with `MAP_NORESERVE`, and use the part of the commit limit that is kept for this capability |

The unused bits stay unused. Vex does not move a Linux bit to a different
number, so a check that a program makes against a Linux mask keeps its answer.

`CAP_DAC_OVERRIDE` still fails an execute check on a file that has no execute
bit for any user. Linux does the same, and it keeps a data file from becoming
executable through the capability alone.

## The sets

`struct __k_caps` holds four masks and a word of flags: `effective`,
`permitted`, `ambient`, `bounding` and `flags`. The `capget(2)` and
`capset(2)` calls copy this structure.

- The effective set gates the checks.
- The permitted set is the ceiling of the effective and ambient sets.
- The ambient set survives the exec of a program that has no setuid or setgid
  bit and no file capabilities.
- The bounding set limits what an exec may grant. Only a lower mask is
  accepted, so it can shrink and never grow.

`capset(2)` needs no capability of its own. The subset rules are the whole
check. It accepts a permitted set that is not larger than the current one, and
a bounding set that is not larger. It accepts effective and ambient sets that
are subsets of the new permitted set. A reserved field or a flag other than
`CAPS_KEEP` fails with `EINVAL`.

`CAPS_KEEP` (`0x0001`) keeps the permitted set when the last user id 0 goes
away. Without it, that change clears the permitted and ambient sets. A change
of the effective id away from 0 clears the effective set. A change to 0 sets
the effective set to the permitted set.

## Program start

An exec works out the new capability sets in this order:

1. It clears the flags. It also clears the ambient set when the program has a
   setuid or setgid bit, or when the file has capabilities.
2. If the new effective id is 0, the permitted set becomes the bounding set,
   and the effective set becomes the permitted set.
3. Otherwise the permitted set becomes the ambient set masked by the bounding
   set, and the effective set becomes the permitted set.
4. The file capabilities add the file permitted set, masked by the bounding
   set. When the file effective flag is set, the effective set becomes the
   permitted set. A file that asks for an effective set with a bit outside the
   bounding set fails the exec with `EPERM`.

Vex has no inheritable capability set, so the inheritable mask of a file never
grants anything.

The kernel sets `AT_SECURE` for the new program when an id differs from the
real id. It also sets it when the real id is not 0 and the permitted set has a
bit that is not ambient. A C library uses it to discard the environment of the
caller.

## File capabilities

The capabilities of a program live in the `security.capability` extended
attribute. The layout is the Linux one, and the fields are little-endian:

| Field | Bytes | Meaning |
| --- | --- | --- |
| `magic_etc` | 4 | Revision in the top byte, flags in the low byte |
| `data[0]` | 8 | Low halves of permitted and inheritable |
| `data[1]` | 8 | High halves of permitted and inheritable |
| `rootid` | 4 | Revision 3 only |

Three revisions exist, and the size tells them apart:

| Revision | Size | Sets |
| --- | --- | --- |
| 1 | 12 | 32-bit, no `rootid` |
| 2 | 20 | 64-bit, no `rootid` |
| 3 | 24 | 64-bit, with `rootid` |

`VFS_CAP_FLAGS_EFFECTIVE` (`0x0001`) in `magic_etc` asks for the permitted set
to be effective at the start. A revision 3 attribute with a `rootid` that is
not 0 names a user namespace, and Vex has none. It reads as no capabilities,
as Linux does.

The VFS removes the attribute when the file is written or truncated, and when
a chown changes its owner. A caller without `CAP_SETFCAP` cannot set it or
remove it.

## Source

- [capability.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/capability.h#L1) — the bits, the sets and the xattr layout
- [cred.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/cred.c#L186) — `cred_capable`, `capset` rules, exec and the parsing of the xattr
- [cred.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/cred.h#L51) — the interface of the credential code
- [sys_cred.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L232) — `capget(2)` and `capset(2)`
- [vnode.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/vnode.c#L605) — the xattr permissions and the VFS checks
- [sys_process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_process.c#L80) — the file capabilities at exec
- [commit.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/commit.c#L15) — the reserve for `CAP_VM_NORESERVE`
