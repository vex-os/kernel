# Kernel architecture

This is a short map of the kernel for contributors. It is not the full design document yet.

## Build and link

Each top directory of `kernel/` (vendored uACPI goes with `firmware`) is compiled into its own static archive. The archives are then linked by
calling `ld` directly with `kernel/arch/<arch>/kernel.ld`, as Clang has no bare-metal driver for amd64. Each subdirectory's `meson.build`
just appends to `kernel_sources`. The `kernel_subsystems` table in `kernel/meson.build` lists the top directories and gives each one the
`KP_SUBSYSTEM` that its `pr_*` calls log as. A new top directory needs an entry there.

## Initcalls

Initcalls replace explicit init sequencing. Subsystems register with `initcall_bsp_<stage>(fn, INIT_FIRST|EARLY|MIDDLE|LATE|LAST)` or
`initcall_smp_<stage>(...)` (`kernel/include/vex/initcall.h`). They are placed in `.initcalls.bsp.*` / `.initcalls.smp.*` sections that the
linker script `SORT`s. The stage order is roughly: early → arch → bootinfo → pmm → slab → mmu → heap → irq → timer → syscall → multicore →
bus → vfs → dev → fs → cpudev → binfmt → services → late.

`kmain()` (`kernel/init/kmain.c`) only runs the BSP initcalls, mounts `root=` from the kernel cmdline, loads `init=` (falling back to
`CONFIG_DEFAULT_INIT`) as the first process, and idles. Init lives as long as the kernel: its death panics by design, and shutdown is a
separate syscall.

## Syscalls

Syscalls register the same way: `define_syscall(sys_foo, SYS_FOO, 2)` puts an entry in a linker section, and `kernel/proc/syscall.c` builds
the dispatch table at the syscall stage. Syscall numbers live in `kernel/include/usr/vex/syscall.h`. The implementations are in
`kernel/proc/sys_*.c`.

A handler is a plain function whose parameters are the registers that the call reads, so its signature is the signature of the call. The
last argument of the macro is the number of those registers. `define_syscall_fd` and `define_syscall_dirfd` are for a call that starts with
a descriptor or with a descriptor and a path. They hand the handler a `file_t*`, or an `int` and the path copied from user space. The macros
and the rule for adding one are in `kernel/proc/syscall_internal.h`.

## Header layout

- `kernel/include/vex/`: kernel-internal API, included as `<vex/...>`.
- `kernel/include/usr/vex/`: the userspace-visible ABI (errno, stat, fcntl, syscall numbers…), with `__k_`-prefixed names. The matching
  `vex/` header includes it and re-exports the plain names, structs as `_t` typedefs (`typedef struct __k_stat stat_t;`).
- `kernel/arch/<arch>/include/arch/`: what the architecture provides to generic code (`<arch/pmap.h>`, `<arch/hart.h>`, …). Generic headers
  such as `vex/pmap.h` include these and declare the arch-neutral interface. Arch-private headers are named `arch/amd64.*.h`.
- A subsystem split over several files keeps what they share in a local `<name>_internal.h` next to them
  (`kernel/device/device_internal.h`, `fs/fat/fat_internal.h`), included as `"<name>_internal.h"`. It is never included from outside its
  directory.

## Generic and arch code

`kernel/arch/amd64/` mirrors the generic directories (`mm`, `proc`, `irq`, `time`, `firmware`, `bus`, `core`) and implements their
arch-specific halves.

## Memory

Three types name an address, so that one kind cannot take the place of another. `user_addr_t` is an address as a system call got it, not
checked yet. `virt_addr_t` is an address in the address space of a `pmap` or a `vmspace`, one that the kernel cannot follow as a pointer.
`phys_addr_t` is a physical address. `vex/address.h` has their operations. A number becomes an address through `user_addr()`,
`virt_addr()` or `phys_addr()`, and an address becomes a number through `.value`. Memory that the kernel can reach, such as the direct map,
the heap and the kernel stacks, is a plain pointer. Only `user_addr_t` takes a plain integer as an argument, as a system call wrapper passes
the register to the handler as it is.

The kernel has two physical page allocators: `pdma` (the first 16 MiB, for ISA DMA) and `pslab` (a free list of single pages). Both names
are terms, not abbreviations. The slab allocator serves objects up to `SLAB_MAX_SIZE` (half a page).

Private writable memory counts against a commit limit (`kernel/mm/commit.c`): the pages of `pslab` minus a reserve for the kernel. A region
takes its pages when it appears (`alloc_region` in `vms.c`) and gives them back when it goes (`free_region`), so `mmap`, `mprotect` and
`fork` fail with `ENOMEM` at the call and not on a page fault. A process with `CAP_VM_NORESERVE` can use `MAP_NORESERVE` to map memory
that does not count. A touch of such memory that finds no page sends `SIGBUS`.

## Device model

`kernel/device/` holds one tree of `device_t` nodes, one per piece of hardware, changed only through `device_worker`. A bus is a node with
`bus_ops_t` (`<vex/device_bus.h>`); each bus lives in `kernel/bus/<bus>/`. Firmware descriptions (`fwnode_t`) come from
`kernel/firmware/<firmware>/`. Drivers declare themselves with `define_driver` (`<vex/driver.h>`), and probe returns a `DRIVER_SCORE_*`.
What userspace sees of a node (block, char, serial, tty, fb) is an interface on top of it, in `dev/`.

## Storage stack

- `dev/` has the block and char interface registries (`block_interface_t`, `char_interface_t`; a filesystem calls the block interface it
  sits on its `volume`), the partition-table parsers (GPT/MBR) and the ATA/ATAPI helpers.
- `drivers/` holds the hardware drivers, one directory per device class (`storage/ahci.c`, `display/limine.c`, `input/ps2kbd.c`). A driver
  that outgrows one file gets its own subdirectory there.
- Class cores such as the tty layer belong in `dev/`, not `drivers/`.
- A device that shows a kernel subsystem to userspace lives with that subsystem, not in `dev/`: `/dev/hwtree` in `device/hwtree.c`,
  `/dev/kplog` in `core/kplog.c`.
- `fs/` has the VFS (vnode, vcache, mount, path, file) plus the devfs, echfs, ext2 and FAT backends.
- Device names look like `ada0n1p1` (used in `ktest/limine.conf.in`).

## Framebuffer console

`kernel/fbcons/` is a console backend that draws on a framebuffer. It is a top directory with its own `Kconfig`, and it builds only with
`CONFIG_FBCONS`. The font is a raw bitmap from `kernel/fbcons/fonts/`, named `<name>_<width>x<height>.bin`, in CP437 order. At build time
`scripts/fbcons/bin_to_ch.py` turns it into `generated.fbcons_font.{c,h}`, and the size comes from the name. A new font needs its file and an
entry in the `FBCONS_FONT` choice. `scripts/fbcons/psf_to_bin.py` converts a PSF font to this format.

## Logging

Use `pr_emerg` … `pr_verbose` (`vex/kprintf.h`). The `loglevel=verbose` cmdline option enables verbose output. Use `panic()` for fatal
errors and `kassert()` for assertions (gated by `CONFIG_KASSERT`).
