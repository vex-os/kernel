# Physical memory

The bootloader gives the kernel a map of physical memory. Two page allocators
split that map. `pdma` owns the low 16 MiB, which ISA DMA can reach. `pslab`
owns single pages above that zone. A slab allocator serves objects of up to
half a page from `pslab` pages, and `kmalloc` builds its heap on `pslab`
pages too.

A frame is one page of `PAGE_SIZE` bytes. One `pframe_t` describes each frame
below the highest usable end.

## The memory map

`memmap` points at the Limine memory map response. The request itself sits in
`.limine_requests`. `init_memmap()` runs at `bootinfo`/`INIT_FIRST` and
panics when the bootloader gives no map or an empty one.

`memmap_is_ram_type()` treats five Limine types as RAM:
`LIMINE_MEMMAP_USABLE`, `LIMINE_MEMMAP_ACPI_RECLAIMABLE`,
`LIMINE_MEMMAP_ACPI_NVS`, `LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE` and
`LIMINE_MEMMAP_EXECUTABLE_AND_MODULES`. `memmap_is_ram()` checks a byte run
against the map and follows the run across entry boundaries. Every byte of
the run must lie in an entry of one of those five types.

Both page allocators take memory from `LIMINE_MEMMAP_USABLE` entries only.
The other four types are not free, although `memmap_is_ram()` accepts them.

```
0x00000000  pdma: bitmap and pages, from USABLE entries
0x00FFFFFF  PDMA_APPROX_END, the end of the DMA zone
0x01000000  pslab zone; pslab excludes the pframe_t array
            each free pslab page points at the next
```

The kernel keeps the bootloader's response, not a copy, so the map must stay
valid for the life of the kernel.

## The frame descriptors

`pframe_of()` indexes a `pframe_t` array with `phys.value / PAGE_SIZE`.
`pframe_phys()` reverses it. `init_pframe()` sizes the array by the highest
usable end in the map. The array therefore also covers a frame in a hole and
a frame that no allocator owns.

The array lives in the first usable entry with room above
`PDMA_APPROX_END`. `init_pframe()` runs at `pmm`/`INIT_EARLY`, before
`pslab`, so `pslab` can keep the array out of its range.

`refcount` is 0 for a free frame. `pslab` sets it to 1 when it gives a page
out, and `pframe_incref()` adds a reference for a mapping or a sharing.
`pframe_decref()` decrements only a count above 1 and returns true when the
count is 1. The caller frees the page at that point. `PFRAME_PSLAB` marks a
frame that `pslab` manages.

## pdma

`pdma` serves ISA DMA, which cannot address memory at or above 16 MiB.
`PDMA_APPROX_END` is `0xFFFFFF`. A bitmap with one bit per page tracks the
zone, and a set bit means free. `init_pdma()` clears the bitmap and then sets
the bits of each usable run, so a hole in the map never becomes allocatable.

The bitmap comes from the first usable entry below `PDMA_APPROX_END` with
room for it, and that entry gives the bitmap pages up. `pdma_numpages` is
rounded up to a multiple of `BITMAP_CHUNK_BITS` pages.

`pdma_alloc_phys()` finds a run of `num_pages` set bits, starting at
`pdma_lastpage` and wrapping to 0 once. It clears the bits, records the new
hint and returns `page * PAGE_SIZE`. `pdma_alloc_virt()` returns the direct
map address of the same pages.

`pdma_free_phys()` sets the bits and lowers `pdma_numused`. `pdma_lock`
protects the bitmap, and alloc and free take it with interrupts off.

## pslab

`pslab` is a free list of single pages. `init_pslab()` adds every usable page
at or above `PDMA_APPROX_END + 1`, less the range that `pframe_storage()`
reports. It runs at `pmm`/`INIT_MIDDLE`, after `pframe` and `pdma`.

A free page holds the next link in its first `sizeof(void*)` bytes, so the
list costs no memory outside the pages. `take_page()` pops a page under
`pslab_lock`, sets `refcount` to 1 and returns the direct map address.

`pslab_free_virt()` asserts `PFRAME_PSLAB`, a `refcount` of 1 and no
`mappings`, sets `refcount` to 0 and pushes the page. `pslab_alloc_phys()`
and `pslab_free_phys()` convert through the direct map.

`pslab` can ask the file cache for pages. When the list is empty,
`pslab_alloc_virt()` calls `fcache_reclaim(RECLAIM_BATCH)` once and retries
if it frees a page. When fewer than `RESERVE_PAGES` pages remain, it reclaims
ahead.

## The slab allocator

`slab_alloc()` serves an object of up to `SLAB_MAX_SIZE`, which is half a
page. The slab table has one entry per power of two from `SLAB_BLOCKSIZE(0)`
to `SLAB_MAX_SIZE`, so nine sizes from 8 to 2048 bytes on amd64.
`init_slab()` runs at `slab`/`INIT_MIDDLE` and starts each slab with one
`pslab` page.

The table of `slab_t` comes from `pdma_alloc_virt()` and stays in the low
zone. A page starts with a `slab_t*`, and the rest of the page is blocks. A
free block holds the next free block in its first `sizeof(void*)` bytes.
`expand_slab()` builds that chain when a slab runs empty.

`slab_alloc()` takes the slab of the exact size when one exists, and
otherwise the first slab whose blocks are large enough. It zeroes `size`
bytes, not the whole block. `slab_realloc()` copies
`min(sl->blocksize, size)` bytes and frees the old block. `slab_free()` finds
the `slab_t*` at the start of the page and pushes the block.

## Who takes pages

- `pdma`: the slab table, the AHCI DMA area, the PATA scratch page and the
  bounce buffer of a block request.
- `pslab`: page tables, `vms` pages, the file cache, `kmalloc`, `kstack` and
  many small buffers.
- `kmalloc` maps its `pslab` pages from `HEAP_REGION_BASE` in `sys_pmap`, and
  describes each block with a `memblock_t` from the slab.
- `kstack` maps `KSTACK_PAGES` `pslab` pages into a slot in
  `KSTACK_REGION_BASE` and leaves the page below them unmapped as a guard.

## Invariants and traps

- A `pframe_t` exists for every frame below the highest usable end, but only
  a frame with `PFRAME_PSLAB` belongs to `pslab`.
- `pdma_free_phys()` accepts any non-null address. It checks neither the zone
  nor the allocation that the address came from. A `pslab` page writes past
  the bitmap. A run that crosses a hole marks the hole free. Both decrement
  `pdma_numused`.
- `pdma_alloc_phys()` returns `phys_addr(0)` for a request of 0 pages and for
  a request larger than the zone.
- A `pslab` page comes back with its old contents, but its first
  `sizeof(void*)` bytes hold the free-list link. Neither page allocator zeroes
  a page. `slab_alloc()` zeroes `size` bytes of a block.
- The `kassert` checks in `pslab_free_virt()` vanish without `CONFIG_KASSERT`.
  A wrong free then corrupts the free list in silence.
- `slab_free()` cannot detect a double free. A second free with no
  allocation between makes a cycle in the free list of the slab.
- `slab_alloc()` returns `nullptr` above `SLAB_MAX_SIZE`. Use `kmalloc` for a
  larger object.
- A guard page is never mapped, so a stack overflow faults at once.
  `kstack_is_guard()` identifies the guard page of a slot in
  `KSTACK_REGION_BASE`.

## Limits

- `pdma` is the only address-limited zone. There is no NUMA node, no
  high-memory zone and no memory hotplug.
- `pdma` cannot serve a page above `PDMA_APPROX_END`. A caller that needs a
  page above the zone uses `pslab`.
- `pslab` has one lock and one list. It has no per-CPU cache and no
  fragmentation control.
- The slab allocator has no constructor, no destructor, no per-object
  metadata and no debugging mode. Size classes are powers of two only.
- The heap does not charge the commit limit. It takes `pslab` pages, so a
  large heap leaves fewer pages for mappings. [Commit accounting](commit.md)
  describes the limit itself.
- `memmap_is_ram()` scans the entries for each run. It has no interval index.

## Source

| File | Contents |
| --- | --- |
| [memmap.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/memmap.c#L54) | The Limine map, the RAM types and the run check |
| [pframe.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/pframe.c#L22) | The frame array, its storage and the reference count |
| [pdma.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/pdma.c#L125) | The DMA zone bitmap and its allocator |
| [pslab.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/pslab.c#L68) | The single-page free list and the reclaim reserve |
| [slab.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/slab.c#L44) | The size classes, the page header and the block lists |
| [kmalloc.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/kmalloc.c#L201) | The heap, its blocks and the coalescing |
| [kstack.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/kstack.c#L72) | The stack slots, the guard page and the slot bitmap |
| [memmap.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/memmap.h) | `memmap` and the two queries |
| [pframe.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/pframe.h) | `pframe_t` and the `PFRAME_*` flags |
| [pdma.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/pdma.h) | `PDMA_APPROX_END` and the pdma calls |
| [pslab.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/pslab.h) | The pslab calls and `pslab_numfree` |
| [slab.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/slab.h) | `SLAB_MAX_SIZE` and the slab calls |
