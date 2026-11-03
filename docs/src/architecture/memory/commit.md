# Commit accounting

Vex reserves physical pages before it lets a process map memory that can
become private and writable. The reservation is a count of pages, not a range
and not an object. One global limit bounds the total of these counts.

A charge that the limit cannot cover returns `-ENOMEM`. A mapping call fails
at that point, not at a later fault. The caller can still handle the error
before it relies on the memory.

Shared anonymous memory and pipe pages use the same counter. The counter thus
covers more than the writable mappings of a process.

## The commit limit

`init_commit()` computes `commit_limit_pages` once, at the `INIT_LATE` stage
of the pmm initcalls. The value is `pslab_numpages` minus a reserve for the
kernel. The reserve is an eighth of the pages, or 4096 pages (16 MiB),
whichever is larger. When the page count is not larger than the reserve, the
limit stays 0 and the kernel logs a warning.

`commit_charged_pages` counts the charged pages. `charge()` updates it with a
compare-and-swap, so any context can charge without a lock. A charge that
fails takes nothing and returns `-ENOMEM`. `commit_charge()` takes pages with
no credit, and `commit_uncharge()` gives them back.

`cap_reserve_pages` is a second slice, inside the limit. It is a 32nd of the
limit, or 256 pages (1 MiB), whichever is larger. It never passes the limit.
A charge with `use_reserve` set may use the whole limit. A charge without
`use_reserve` stops at `commit_limit_pages - cap_reserve_pages`.

```
0            limit - cap_reserve         limit
+--------------------+------------------------+
| any charge         | use_reserve charges    |
+--------------------+------------------------+
|<------------- commit_limit_pages ----------->|
```

`commit_charge_replacing()` takes the pages that the caller will give back as
credit. The total may then pass the limit by that many pages for a moment. A
plain charge fails while the total is over the limit, until the caller
uncharges the replaced pages. `vms_map_replace()` charges the new region, then
removes the old one.

## What takes pages

`alloc_region()` in `vms.c` charges a region when `region_counted()` is true.
That needs four conditions. The region is neither device memory nor shared. It
does not carry `VMS_NORESERVE`, and its protection has `VPROT_WRITE`. The
charge is `region_pages()`, the page count of the range. `free_region()` gives
the pages back.

- A private anonymous writable mapping charges for its whole range.
- A private writable file mapping charges too, because a write can make a
  private page. A read-only private mapping charges nothing until a
  `mprotect` adds write.
- A stack charges for its initial range at `exec`, which is at most 128 KiB.
  `grow_stack_locked()` charges the pages that a growth adds, at the fault
  that grows it.
- `fork` charges the private writable regions of the child again. The frames
  stay shared until a write, so the pair charges twice for the same frames.
- A tmpfs file charges for each page that its size grows by.
  `shmem_file_create()` sets the size of its file, so the whole file charges
  at creation.
- A pipe charges one page for each page that it holds.

`mprotect` can move a charge when it changes the write bit. `vms_protect()`
charges the regions that gain a charge and uncharges those that lose one.
`mremap` charges the pages that a growth adds. A move charges the new region,
with the pages of the old one as credit. The old region uncharges when it
goes.

## MAP_NORESERVE

`sys_mmap()` sets `VMS_NORESERVE` only when the calling thread holds
`CAP_VM_NORESERVE`. Without the capability the kernel accepts the flag and
ignores it. A program that passes the flag out of habit gets the same charge
as without it.

A region with `VMS_NORESERVE` takes nothing from the limit. An anonymous page
of it comes from `pslab` at the fault. When `pslab_alloc_phys()` returns null,
the fault fails with `SIGBUS` and `BUS_ADRERR`. The fault does not read the
commit limit, because the region has no charge.

The capability has a second effect. `may_use_reserve()` reads it at each
mapping charge, so a mapping without `MAP_NORESERVE` may use the cap reserve.

`fork` and `mremap` copy the `noreserve` flag of a region. They do not check
the capability again.

## Shared anonymous memory

`MAP_SHARED | MAP_ANONYMOUS` makes a file in the internal tmpfs.
`map_shared_anonymous()` gives the page-aligned length as the size of the
file. `tmpfs_charge()` then charges the whole size at once.

The mapping is a shared file mapping, so the region takes no charge. The
charge belongs to the file, not to the region. A `fork` copies the region but
not the charge, so two processes share one charge. See
[Shared memory](../ipc/shmem.md).

`MAP_NORESERVE` does not remove this charge. The file takes its whole size
from the limit when the mapping appears. A `CAP_VM_NORESERVE` holder can still
make that charge use the cap reserve.

## Invariants

- The charged total stays at or below the limit, except while a
  `commit_charge_replacing()` credit is outstanding.
- `commit_uncharge()` gives back pages that an earlier charge took. Its
  `kassert` fails when the counter is smaller.
- The reserve check uses the credentials of the calling thread, not those of
  the process that owns the mapping.
- A split keeps the total charge. `split_region()` copies the accounting
  fields, and each part uncharges its own pages.
- The kernel fixes the limit at boot. It does not follow the free pages of
  `pslab`, and kernel allocations come from the same pool. The reserve is
  headroom, not a hard boundary.

## Traps

- `MAP_NORESERVE` without `CAP_VM_NORESERVE` is not an error. The mapping
  counts like any other.
- A shared anonymous mapping with `MAP_NORESERVE` still charges its whole
  size.
- A charge is not an allocation. The page does not exist until the fault, and
  a fault can still fail with `SIGBUS` when `pslab` is empty.
- `fork` charges the private writable memory of the child again. A process
  with a large private writable mapping can fail to fork at about half of the
  commit limit.
- A stack growth over the limit fails at the fault, with `SIGSEGV`. Only the
  initial range is charged at `exec`.
- `tmpfs_charge()` returns `-ENOSPC` when its page limit or the commit limit
  is full. `shmem_file_create()` turns that into `-ENOMEM`.
- `commit_charged_pages` uses relaxed atomics, so a concurrent reader can see
  a stale value. `tmpfs_statvfs()` reads it for the free space report.

## Limits

- There is no reservation object and no record per mapping. The accounting is
  two counters and two reserved slices.
- There is no swap and no overcommit mode.
- Only a `CAP_VM_NORESERVE` holder gets the effect of `MAP_NORESERVE`.
- Device memory takes no charge. A device mapping is always shared.
- The kernel does not enforce `RLIMIT_AS`.
- There is no reclaim, and no charge shrinks under memory pressure. A charge
  goes away only when the pages are freed.
- The unit is `PAGE_SIZE`. A user page is never huge.

## Source

| File | What it holds |
| --- | --- |
| [commit.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/commit.c#L23) | the counters, `charge()` and the initcall |
| [commit.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/commit.h#L9) | the charge interface and its contract |
| [vms.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/vms.c#L710) | `alloc_region()`, `free_region()` and the stack growth |
| [sys_mman.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L174) | the `MAP_NORESERVE` test and the shared anonymous path |
| [shmem.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/shmem.c#L23) | the unnamed tmpfs file of a shared mapping |
| [shmem.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/shmem.h#L8) | the contract of `shmem_file_create()` |
| [tmpfs.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/tmpfs/tmpfs.c#L221) | `tmpfs_charge()` and its reserve rule |
| [tmpfs_vnode.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/tmpfs/tmpfs_vnode.c#L141) | the size change that charges and uncharges |
| [isr.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/isr.c#L98) | the signal for a fault that fails |
| [pipe.c](https://github.com/vex-os/kernel/blob/main/kernel/ipc/pipe.c#L71) | one charge for each pipe page |
