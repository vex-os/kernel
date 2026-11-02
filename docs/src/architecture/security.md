# Security

Vex has no security module and no policy engine. A privileged decision reads
the credentials of the calling thread and compares them with the object. The
credentials say who the thread is, and which capabilities it holds. The kernel
also contains a few cryptographic primitives for its own use.

Capabilities are one field of the credentials. Their bits and sets have their
own page: [Capabilities](../reference/capabilities.md).

## The credential object

`credentials_t` holds a reference count. It holds the real, effective and
saved user and group ids, and four capability masks with a flag word. It also
holds a sorted array of supplementary group ids. The array is part of the
allocation, so the structure has a size that depends on the number of groups.

The object never changes after a process publishes it. A call that changes an
id or a capability takes the `credentials_lock` of the process, copies the
object, changes the copy, and swaps it in with `process_set_cred()`. The old
object loses the reference that the process held, and it dies when its last
reference goes. A small copy comes from the slab, and a copy with many groups
from `kmalloc`.

`kernel_credentials` is a static object with every capability and no group. It
never dies. The kernel uses it for its own file operations, and a kernel
thread starts with it.

## Which copy a thread uses

A thread caches a reference in `thread->credentials`. Another thread of the
same process can replace the credentials between two system calls, so the
kernel checks the cache at every system call entry. `catch_up_cred()` compares
the cached object with `process->credentials`, takes the new one when they
differ, and drops the old reference.

`process_get_cred()` returns a new reference. A caller that keeps the pointer
across a call that can block holds that reference, so a concurrent change
cannot free the object under it.

A fork gives the child a reference to the credentials of the parent. An exec
builds a copy, applies the setuid and setgid bits and the file capabilities,
and swaps it in.

## Ids

Each of the user and group ids has three values: real, effective and saved. A
process may set one of them to its current value or to one of the other two.
`CAP_SETUID` or `CAP_SETGID` removes that limit. The saved id keeps a program
able to return to the id that started it.

A change of a user id also moves the capability sets. The rules are in
[Capabilities](../reference/capabilities.md#the-sets).

The supplementary groups are sorted in ascending order, and `cred_in_group()`
finds one with a binary search. `CONFIG_NGROUPS_MAX` sets the limit, and
`setgroups(2)` needs `CAP_SETGID`.

## Access checks

`vnode_access()` makes the decision for a file:

1. `CAP_DAC_OVERRIDE` grants the request. An execute request still fails when
   the file has no execute bit for any user and is not a directory.
2. The owner bits apply when the effective user id is the owner.
3. The group bits apply when the effective group id or a supplementary group
   matches.
4. The other bits apply in every other case.

The path walk and the file operations use the effective ids. `access(2)` is
the exception: it checks the real ids, unless `AT_EACCESS` is set.
`cred_copy_real()` makes that copy. It also makes the effective set equal to
the permitted set when the real user id is 0, and empty otherwise.

`chmod`, `chown` and the timestamp calls compare the effective user id with
the owner, and a capability lifts each comparison. The bits table of
[Capabilities](../reference/capabilities.md#the-bits) names the capability
for each operation.

## Program start

`exec` copies the credentials. It applies the setuid bit to the effective user
id. It applies the setgid bit to the effective group id when the group execute
bit is set. The saved ids follow the effective ids. `cred_exec_caps()` then
works out the capability sets. The kernel reports `AT_SECURE` to the new
program when an id differs from the real id. It also reports it when the real
id is not 0 and the permitted set has a bit that is not ambient.

## Cryptography

There is no general cryptographic interface. Three primitives exist, and each
one serves one subsystem:

- BLAKE2s hashes the entropy pool and mixes a seed into the key.
- ChaCha20 generates the random bytes from that key. Both primitives live in
  [Random numbers](random.md).
- CRC32 is a checksum, not a security primitive. The GPT parser checks the
  header and the entry array with it, and the device-name code hashes a prefix
  with it.

The CRC32 code builds a table for a polynomial and an initial value, so a
second user can pick its own.

## Traps

- The cached credentials of a thread can be one change behind. Only the system
  call entry brings them up to date.
- A `credentials_t*` that crosses a blocking call needs its own reference.
- `cred_capable()` reads the effective set only.
- Never copy `credentials_t` with `sizeof(credentials_t)`. The group array is
  outside it. Use `cred_copy()`.
- A kernel thread has no process. A system call from one returns `ENOSYS`.

## Source

- [cred.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/cred.h#L10) — the object and its interface
- [cred.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/cred.c#L110) — the copy, the reference count, the id rules and the exec transition
- [sys_cred.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_cred.c#L46) — the id system calls
- [syscall.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/syscall.c#L22) — the refresh of the cached credentials
- [process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/process.c#L753) — the swap of the published object
- [vnode.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/vnode.c#L1270) — the access check
- [sys_path.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_path.c#L200) — `access(2)` and the real ids
- [blake2s.c](https://github.com/vex-os/kernel/blob/main/kernel/crypto/blake2s.c), [chacha20.c](https://github.com/vex-os/kernel/blob/main/kernel/crypto/chacha20.c) and [crc32.c](https://github.com/vex-os/kernel/blob/main/kernel/crypto/crc32.c) — the primitives
