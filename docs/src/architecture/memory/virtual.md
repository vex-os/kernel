# Virtual memory

A program names addresses that no hart can follow as pointers. The kernel
must give each page a frame at the first access. It cannot know in advance
which pages the program will touch. It must also reach any RAM frame from
its own code, at any time.

Three objects divide the work:

- `pmap_t` is a hardware page table.
- `vmspace_t` is the set of ranges that a program may name, as
  `vm_region_t` records them.
- The HHDM, the direct map, makes every RAM frame a plain pointer.

The page table and the region list have their own locks. The kernel half of
every `pmap_t` holds the same mappings, and the HHDM belongs to all address
spaces. The page table is the amd64 one. A port implements the same `pmap_*`
functions for its own table format.

## Page tables

`pmap_t` holds a root page (`virt` and `phys`), `num_levels`, a `lock`, and
the counters `resident_pages` and `peak_pages`.

`pmap_create()` takes a root page from `pslab` and copies the entries from
`AMD64_PAGEMAP_START_KERN` through `PAGEMAP_SIZE - 1` from `sys_pmap`. Every
address space therefore shares the kernel half and starts with an empty user
half.

The copy takes entry values, not tables. `init_mmu()` creates every kernel
top-level entry first, because a copy does not propagate later additions.

```
root table entry
             +-----------------------------------+
  0 .. 255   | user half: one program            |
             | pmap_create leaves it empty       |
             +-----------------------------------+
256 .. 511   | kernel half: shared by every pmap |
             | arch_pmap_kcopy copies it from    |
             | sys_pmap when the pmap is made    |
             +-----------------------------------+
```

`pmentry_walk()` descends from the root to a leaf or a large leaf. It
allocates the missing tables on the way. `pmentry_get()` takes each table
page from `pslab`. `split_large_nolock()` replaces one large leaf with a
table of 512 entries, so that one page below it can change.

`pick_leaf_level()` keeps a user page at 4 KiB. A 2 MiB or 1 GiB leaf needs
virtual and physical addresses aligned and the range at least that size.
A 1 GiB leaf needs `CPU_FEATURE_PAGE_1G`.

`pmentry_setup()` encodes the `VPROT_*` bits. A user page with no right
clears `AMD64_PML_PRESENT` and sets `AMD64_PML_PROTNONE`, because a present
x86 page is always readable. `pmentry_valid()` accepts either bit.

`pmap_release()` frees the tables of the user half only. The kernel half
belongs to `sys_pmap`.

## Address spaces and regions

`vmspace_t` holds `regions`, a list sorted by `start`, its `pmap` and a
`lock`. `find_region()` walks that list.

`vms_map()` inserts a `vm_region_t` and maps no page. The first access of
each page faults. `vms_unmap()` splits the list at both ends with
`split_region()` and drops the whole regions between them.

`vm_region_t` keeps `vprot` without `VPROT_USER`, because every user page
has that bit. `file` and `file_offset` make a file mapping. `shared`,
`may_write`, `device`, `noreserve`, `grows_down` and `grow_floor` describe
how the range faults and what it costs.

The [commit limit](commit.md) pays for a private writable region in
`alloc_region()`, and `free_region()` gives the pages back. A read-only,
shared, device or `VMS_NORESERVE` region takes nothing.

A `pmap` answers a virtual address with a frame. The rmap answers a frame
with its mappings. `rmap_map()` writes the page table entry and adds an
`rmap_entry_t` to the `pframe_t`. A frame then knows every `pmap` and `virt`
that holds it. A device page has no `pframe`, so it stays out of the rmap.

`rmap_pin()` returns a frame with a new reference. It also sets `sole` when
the frame has one mapping and no file cache (`PFRAME_CACHE`). A write fault
uses that answer to choose between a patch and a copy.

`vms_map_any()` searches down from `VMS_MMAP_TOP`. The lowest address is
`VMS_MMAP_MIN`, which keeps a null pointer dereference faulting.

## Faults

`isr.c` reads `CR2` and the error code, enables interrupts, and calls
`vms_fault()` with `VPROT_READ`, `VPROT_WRITE` or `VPROT_EXEC`. A fault from
the kernel itself panics.

`fault_locked()` runs under `vmspace->lock` and looks for the region. A page
that no region contains may extend a stack down (`grow_stack_locked()`). The
growth stops at `grow_floor`, and it needs a free `STACK_GUARD_BYTES` gap
above the mapping below.

Then the fault checks the access against `region->vprot`. A device region
goes to `fault_device_locked()`, which maps the frame that
`vnode_resolve_page()` returns, with the cache bits of the node.

Anonymous memory takes a page from `pslab`, zeroes it and maps it with
`rmap_map()`. A file calls `vnode_fault()`, which lets the filesystem map the
page. A private page of a file starts read-only. A write to it reaches
`unshare_locked()`. It patches the entry when the frame is sole. Otherwise
it copies the page and calls `rmap_replace()`.

A shared page becomes writable only on a write fault, and that fault marks it
dirty.

A successful fault returns `VMS_FAULT_MINOR` or `VMS_FAULT_MAJOR`, and the
handler counts it. `-EFAULT` becomes `SEGV_MAPERR` and `-EACCES` becomes
`SEGV_ACCERR`. Any other error becomes `SIGBUS` with `BUS_ADRERR`.

`vms_clone()` maps every RAM frame of the parent read-only in the child. A
private writable region loses write in the parent too, so both sides copy on
the next write.

## The direct map

`hhdm_base` comes from the Limine HHDM response. `phys_to_virt()` adds that
offset and `virt_to_phys()` subtracts it.

`map_hhdm()` in `mm/pmap.c` walks `memmap` for `memmap_is_ram_type()`
entries, merges runs that touch, and calls `pmap_map_range()` with
`VPROT_RW`. It panics when a run fails. The mapping lands in `sys_pmap` in
the kernel half, so every `pmap_t` shares it.

The HHDM covers RAM only. Device registers live in the window at
`MMIO_REGION_BASE`. `mmio_map()` finds a gap there and maps the range into
`sys_pmap`. It refuses a physical range that another mapping holds with a
different memory type. `mmio_unmap()` keeps the entry until the last
reference goes.

A device page of a user mapping is not in the HHDM. The kernel reaches it
through the user mapping, and only while that address space is loaded.

## Invariants and traps

- The locks nest in one order: `vmspace->lock`, then the rmap lock of the
  frame, then `pmap->lock`. The rmap lock runs with interrupts off.
- `pmap_flush()` invalidates one address on the local hart only.
  `tlb_shootdown()` raises `tlb_generation` and sends an IPI to every other
  hart, and each one flushes its CR3 in `tlb_poll()`.
- A `_noflush` call leaves the old translation in a TLB. The caller runs
  `tlb_shootdown()` before the old entry can matter, or it knows that no
  hart used the address.
- `vms_release()` flushes nothing. No hart may have the pmap loaded.
- `pmap_unmap_range_noflush()` clears a large leaf only when the range
  covers all of it. It splits a leaf that the range covers in part.
- A frame that `rmap_unmap()` returns is the caller's reference. The pmap
  does not free it. `put_frame()` does.
- `vms_clone()` takes only the lock of the source and needs
  `vmspace->regions == nullptr`.
- A file region that `vms_release()` frees goes to `stale_regions` and dies
  in `release_thread`, because `file_close()` can sleep.
- `type_conflicts_nolock()` refuses one physical range with two memory
  types. x86 leaves the result undefined.

## Limits

- Only amd64 provides `arch_pmap_*`. The kernel detects 3, 4 or 5 paging
  levels from the bootloader response.
- A user page is always 4 KiB. There is no huge page for user memory.
- The page tables have no reclaim, and there is no swap. A table page
  returns to `pslab` only when the pmap goes.
- A device region works in the loaded address space only. The case of
  another one is marked `UNDONE` in `access_begin_locked()`.
- `vms_protect()` cannot add write where `may_write` is false, and it
  removes write from a shared page.
- `locked_bytes` counts `mlock` requests for `RLIMIT_MEMLOCK`. It pins
  nothing.

## Source

| File | What it holds |
| --- | --- |
| [mm/pmap.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/pmap.c#L228) | `pmap_create()`, the walk, the leaf choice and the HHDM setup |
| [vex/pmap.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/pmap.h) | the pmap interface and the flush rules |
| [arch/pmap.h](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/include/arch/pmap.h#L172) | the amd64 entry bits and the `pmentry_*` encoders |
| [amd64/mm/pmap.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/mm/pmap.c#L70) | the root page, the level count and the kernel-half copy |
| [vms.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/vms.c#L388) | regions, faults, remap, protect and the user copies |
| [vms.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/vms.h#L32) | the `vmspace_t` and `vm_region_t` fields |
| [rmap.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/rmap.c#L67) | the reverse map, the pins and the resident page counts |
| [rmap.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/rmap.h#L18) | the preconditions of the rmap calls |
| [hhdm.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/hhdm.c#L13) | reads `hhdm_base` from Limine |
| [hhdm.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/hhdm.h#L9) | `phys_to_virt()` and `virt_to_phys()` |
| [mmio.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/mmio.c#L123) | the MMIO window, its reuse and its memory types |
| [tlb.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/tlb.c#L39) | the shootdown and the generation counter |
| [isr.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/isr.c#L88) | the page fault entry and the signal choice |
| [fcache.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/fcache.c#L785) | the file cache side of a file fault |
| [vprot.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/vprot.h#L3) | the permission and memory type bits |
