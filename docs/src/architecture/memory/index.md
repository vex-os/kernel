# Memory

The kernel gives each address one of three kinds and each frame one
allocator. The kinds stop a physical address from becoming a pointer by
mistake. The commit accounting rejects a mapping that the limit cannot
cover.

`user_addr_t` is an address as a system call received it, not checked
yet. `virt_addr_t` is an address in an address space, which the kernel
cannot follow as a pointer. `phys_addr_t` is a physical address.

Three allocators hand out memory. `pdma` owns the frames below
`PDMA_APPROX_END`, which ISA devices can reach. `pslab` owns single
frames above that range. The slab allocator serves objects up to
`SLAB_MAX_SIZE`. `kmalloc` builds the kernel heap from `pslab` pages.
`pdma` serves a run of pages. `pslab` serves one page at a time.

An address space holds user translations below `pmap_user_end` and the
shared kernel mappings above it. `pmap_create` copies the kernel half of
the top-level entries from `sys_pmap`. A later kernel mapping does not
reach the new address space.

## What they share

The direct map reaches every range that `memmap_is_ram_type` accepts,
at `hhdm_base`. `phys_to_virt` and `virt_to_phys` convert between the
two names. Each frame below the top of usable RAM has a `pframe_t` from
`pframe_of(phys)`. It holds the reference count of the frame and the
translations that reach it.

Four rules hold across the subsystem.

An address of one kind never takes the place of another. A number
becomes an address through `user_addr()`, `virt_addr()` or
`phys_addr()`. A `virt_addr_t` becomes a pointer only through
`virt_addr_pointer`, and only while its address space is current.
Memory that the kernel owns, such as the heap and the kernel stacks,
stays a plain pointer.

A frame returns to `pslab` only when its last user drops it. A
`pmap_unmap` call removes one translation and frees no frame. The
reverse map in `rmap.c` lists the translations of a frame. A write fault
uses it to tell whether it must copy.

A change to a translation that another hart may have cached needs
`tlb_shootdown`. The `pmap_*_noflush` calls change the tables and leave
the flush to the caller. A caller that unmaps many pages can flush once
for a batch.

The kernel charges a private writable mapping when it appears, not when
a page faults. `counts_commit` selects the regions that count: not device,
not shared, not `VMS_NORESERVE`, and writable. So `mmap`, `mprotect` and
`fork` fail at the call, not at the first touch. A stack is the
exception: it grows on a fault and charges then. A shared anonymous
mapping is a file with no name. The file takes its whole size from the
commit limit, so the region does not count.

## Pages

- [Address types](addresses.md) — the three address types, the
  `_Generic` operations in `vex/address.h` and the pointer rule.
- [Physical memory](physical.md) — the memory map, `pdma`, `pslab`, the
  `pframe_t` array, the slab allocator and `kmalloc`.
- [Virtual memory](virtual.md) — `pmap_t`, `vmspace_t` and
  `vm_region_t`, the user half and the direct map.
- [Commit accounting](commit.md) — `commit_charge`, the kernel reserve,
  `VMS_NORESERVE` and shared anonymous memory.

## Limits

The kernel has no swap, so only the file cache returns frames under
pressure, from `fcache_reclaim`.

The kernel fixes `commit_limit_pages` at boot. It is `pslab_numpages`
minus the reserve, so free pages do not raise it. The reserve covers the
slab, the kernel stacks and the page tables. They take pages from
`pslab` without a charge.

`mlock` counts bytes in `vmspace.locked_bytes` against
`RLIMIT_MEMLOCK` and pins nothing.

The pmap code is generic, but only the amd64 page tables have an
implementation.
