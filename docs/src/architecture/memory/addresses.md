# Address types

An address is a number, and C lets one number stand for any other. A user
address, a virtual address and a physical address are not interchangeable.
The same value names different memory in each. Vex gives each kind its own
type, so that the compiler rejects a mix at the call.

The three types are the same size. Each holds one `uintptr_t`, and each passes
in one register. The type names the kind of the address. It does not check
that the address is mapped or that it is in range.

## The three kinds

`user_addr_t` is an address as a system call got it, before any check. It is a
transparent union with one member, `uintptr_t value`. A `define_syscall`
handler takes it as a parameter. The dispatcher passes the raw register, so no
cast is needed.

`virt_addr_t` and `phys_addr_t` are structs with one `uintptr_t value` field.
Their shape is the same, and the types are still separate. A virtual address
cannot take the place of a physical one.

A `virt_addr_t` is an address in the address space of a `pmap_t` or a
`vmspace_t`. The kernel cannot follow it as a pointer. A `phys_addr_t` is an
address in physical memory. The kernel reaches RAM through the direct map.

## Operations

`DEFINE_ADDRESS_OPS(kind, type)` in `address.h` defines the same set for the
three kinds:

- `<kind>_addr()` makes an address from a `uintptr_t`.
- `<kind>_addr_add()` and `<kind>_addr_sub()` move it by a `size_t` offset.
- `<kind>_addr_diff()` gives the distance from a low address to a high one.
- `<kind>_addr_equal()`, `_less()`, `_less_equal()`, `_greater()` and
  `_greater_equal()` compare two addresses of the kind.
- `<kind>_addr_min()` and `<kind>_addr_max()` pick one of two.
- `<kind>_addr_null()` and `<kind>_addr_valid()` test for zero.

All of them are `static __always_inline __nodiscard`. An address becomes a
number through its `value` field.

The `address_*` macros dispatch with `_Generic` on the first argument. They
are `address_add()`, `address_sub()`, `address_diff()`, `address_equal()`,
`address_less()`, `address_less_equal()`, `address_greater()`,
`address_greater_equal()`, `address_min()`, `address_max()`, `address_null()`
and `address_valid()`. A second argument, where there is one, is an address of
the same kind. The exception is the `size_t` offset of `address_add()` and
`address_sub()`.

Page operations live in `page.h`, because they need `PAGE_SIZE`:
`<kind>_addr_page_align()`, `<kind>_addr_page_align_up()` and
`<kind>_addr_page_offset()`. The generic `page_align()`, `page_align_up()`
and `page_offset()` also accept a plain number through their `default:`
branch. The first two give a `uintptr_t` for a plain number, and
`page_offset()` gives a `size_t`.

## Between the kinds

`user_addr_to_virt()` names a user address as one in the address space of a
`vmspace_t`. It changes no bit and checks nothing. The `vms_*` functions check
the range.

The kernel reaches a user page through the direct map, not by following the
user address. `access_begin_locked()` in `vms.c` pins the page, calls
`phys_to_virt()` on the frame, and adds `page_offset(virt)`. Device memory is
the exception. It is not in the HHDM, so the code calls `virt_addr_pointer()`
on the user address. It does so only when that address space is current.

`virt_addr_from_pointer()` and `virt_addr_pointer()` convert between a kernel
pointer and a `virt_addr_t`. The result of `virt_addr_pointer()` has a meaning
only in the address space that is current.

`phys_to_virt()` and `virt_to_phys()` in `hhdm.h` convert between a
`phys_addr_t` and a kernel pointer through `hhdm_base`. They return no
`virt_addr_t`. They do not check that the address is RAM.

`pmap_resolve_address()` returns the `phys_addr_t` that a `virt_addr_t` maps
to, or `phys_addr(0)`. `vms_pin()` and `vms_touch()` return a `phys_addr_t`
for an address in a `vmspace_t`, with the page offset added.

## The user half

The user half of the address space ends at `pmap_user_end()`. The function
computes the top from `pmap->num_levels`. Four levels give
`0x0000800000000000`. A start at or above that end is not a user address, and
neither is a length that passes it.

`user_range_ok()` compares a `virt_addr_t` start and a `size_t` size with the
end. `user_range_end()` in `sys_mman.c` does the same for a range that must be
page aligned, and it rejects a zero length.

`VMS_MMAP_TOP` is `0x0000600000000000`, and `VMS_MMAP_MIN` is
`0x0000000000010000`. A placement without a fixed address searches down from
the first and stops at the second. Neither is a limit on a fixed mapping.
`vms_map()` accepts one up to `pmap_user_end()`. The user stack ends at
`ELF64_STACK_TOP`, `0x0000700000000000`, which is above `VMS_MMAP_TOP`.

## Invariants and traps

- `address_diff(high, low)` is valid only when `high` is not below `low`.
  The subtraction is unsigned, so the other order gives a huge `size_t`.
- `address_add()` and `address_sub()` do not detect a wrap. A `size_t` offset
  can wrap past zero or past the top of the address space.
- Zero is the failure value of the physical allocators. `pdma_alloc_phys()`
  and `pslab_alloc_phys()` return `phys_addr(0)`, and the callers test it with
  `address_null()` or `address_valid()`.
- For a user address, zero is a hint and not a failure. `sys_mmap()` tests
  `address_valid(addr)` to tell "no hint" from a hint. `sys_mmap()` still
  accepts a `MAP_FIXED` mapping at address 0.
- `user_addr_to_virt()` and `virt_addr_pointer()` check nothing. An address
  that the `vmspace_t` does not map becomes a pointer all the same.
- `address_valid()` says only that the value is not zero. It does not say that
  the address is mapped, in range, or in RAM.
- A check of the start of a range does not cover the bytes after it.
  `in_user_space()` in `sys_thread.c` compares one `user_addr_t` with
  `pmap_user_end()`, and the object at that address can still cross the end.
- `phys_to_virt()` accepts any value. A `phys_addr_t` outside RAM gives a
  pointer outside the direct map.

## Limits

- There is no type for a range of addresses. Each call takes a start and an
  end of one kind, or a start and a `size_t` size.
- The code provides no operation that turns a `phys_addr_t` into a
  `virt_addr_t`. It goes through a kernel pointer with `phys_to_virt()` and
  `virt_addr_from_pointer()`.
- There is no 32-bit user address type. amd64 is the only implemented
  architecture.
- The userspace ABI does not use these types. A user address in a structure
  under `include/usr/` is a `__k_uintptr_t`.
- The address types do not keep a reference, a mapping or a lock. A
  `virt_addr_t` does not say which `pmap_t` gives it a meaning.

## Source

| File | Holds |
| --- | --- |
| [types.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/types.h#L58) | the three types, the transparent union and the size checks |
| [address.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/address.h#L9) | `DEFINE_ADDRESS_OPS`, the `address_*` macros and the pointer helpers |
| [page.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/page.h#L38) | the page operations and their plain-number branches |
| [hhdm.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/hhdm.h#L9) | `phys_to_virt()` and `virt_to_phys()` through `hhdm_base` |
| [vms.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/vms.h#L12) | `VMS_MMAP_TOP`, `VMS_MMAP_MIN` and the `vmspace_t` interface |
| [arch/pmap.h](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/include/arch/pmap.h#L76) | `pmap_user_end()` from `pmap->num_levels` |
| [usr/vex/types.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/types.h#L90) | `__k_uintptr_t`, the ABI form of a user address |
| [sys_mman.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L43) | `user_range_end()` and the mmap family |
| [vms.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/vms.c#L175) | `user_range_ok()`, `vms_copyin()` and the direct-map access |
| [pmap.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/pmap.c#L420) | `pmap_resolve_address()` |
| [pdma.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/pdma.c#L63) | `pdma_alloc_phys()`, which returns `phys_addr(0)` on failure |
| [pslab.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/pslab.c#L93) | `pslab_alloc_phys()`, which returns `phys_addr(0)` on failure |
