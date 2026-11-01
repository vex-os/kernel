# Source map

The kernel lives in `kernel/`. Each top directory is one subsystem, one
archive and one log tag. This page explains the layout, the rules that keep
the directories apart, and where new code belongs.

## The tree

```
kernel/
|-- arch/amd64/   paging, CPU, IRQs, timers, ACPI glue
|-- bus/          PCI, ISA and IDE buses
|-- core/         kprintf, kplog, consoles, panic, cmdline
|-- crypto/       blake2s, chacha20, crc32
|-- dev/          block, char, fb, serial and tty interfaces
|-- device/       device tree, binding, resources, /dev/hwtree
|-- drivers/      storage, display, input, serial hardware
|-- exec/         binfmt registry and the ELF64 loader
|-- fbcons/       framebuffer console and fonts
|-- firmware/     fwnode providers, ACPI through uACPI
|-- fs/           VFS and the devfs, echfs, ext2, fat, tmpfs backends
|-- include/      the kernel API and the user ABI
|-- init/         kmain and the initcall runner
|-- ipc/          pipes
|-- irq/          IRQ core and interrupt controllers
|-- libk/         freestanding C support
|-- mm/           physical and virtual memory
|-- proc/         processes, threads, signals, syscalls
|-- random/       entropy sources, /dev/random, /dev/urandom
`-- time/         clocksources, clock events, RTC, wallclock
```

| Directory | What lives there |
|---|---|
| `arch/amd64/` | The arch half of `mm`, `proc`, `irq`, `time`, `firmware`, `bus` and `core`, plus the linker script |
| `bus/` | One directory per bus: `pci/`, `isa/`, `ide/` |
| `core/` | Kernel messages, consoles, panic, the command line, the version |
| `crypto/` | The hashes and ciphers that other subsystems use |
| `dev/` | The interface registries that userspace sees, partition tables, ATA helpers, device names |
| `device/` | The device tree, driver binding, resources, the `/dev/hwtree` device |
| `drivers/` | Hardware drivers, one directory per class |
| `exec/` | The binary format registry and the ELF64 loader |
| `fbcons/` | A console backend that draws on a framebuffer |
| `firmware/` | What the firmware describes, including ACPI |
| `fs/` | The VFS and the filesystem backends |
| `init/` | `kmain()` and the code that walks the initcall sections |
| `ipc/` | Inter-process communication. Pipes today |
| `irq/` | Interrupt delivery and the interrupt controller interface |
| `libk/` | The freestanding functions that the kernel needs from C |
| `mm/` | Page allocators, page tables, the heap, commit accounting |
| `proc/` | Processes, threads, the scheduler, signals, credentials, system calls |
| `random/` | The random pool and its sources |
| `time/` | Timekeeping and timers |

## Archives

Each top directory builds into its own static archive. The table in
`kernel/meson.build` names the directories:

- The key is the directory, such as `fs` or `arch/amd64`.
- The value is the log tag, such as `KP_FS`, that every `pr_*` call in that
  archive uses.

The archive receives the tag with `-DKP_SUBSYSTEM=<tag>`. A file that moves to
another top directory logs under the new tag.

Two details follow from the table:

- A new top directory needs a row. Without one, the directory is not built.
- `fbcons` is a top directory that logs as `KP_CORE`. `firmware` also builds
  the vendored uACPI sources into its archive.

The build links the archives with `ld --whole-archive`. Every object then
survives the link. That is what keeps the registration sections below intact.

## Sections, not calls

A top directory does not call another directory's init function. It registers
an entry in a linker section, and one generic file walks that section. Four
registries work this way:

| Registry | Macro | Section | Walker |
|---|---|---|---|
| Initcalls | `initcall_bsp_*`, `initcall_smp_*` | `.initcalls.bsp.*`, `.initcalls.smp.*` | `kmain()` and the per-hart code |
| System calls | `define_syscall`, `define_syscall_fd`, `define_syscall_dirfd` | `.syscalls` | `proc/syscall.c` |
| Drivers | `define_driver` | `.drivers` | The device core |
| Random sources | `define_random_source` | `.random_sources` | `random/random_source.c` |

The linker script keeps these sections and sorts the initcalls by name. The
name starts with a stage number, so the sort gives the boot order. The order
is: early, arch, bootinfo, pmm, slab, mmu, heap, irq, timer, syscall,
multicore, bus, vfs, dev, fs, cpudev, binfmt, services, late.

This pattern is the reason the directories can stay independent. It also
means that the boot order is data, not code: a new initcall belongs where its
stage says, not where a call site says.

## Headers

| Path | Role |
|---|---|
| `kernel/include/vex/` | The kernel API, included as `<vex/...>` |
| `kernel/include/usr/vex/` | The user ABI: errno, stat, fcntl, syscall numbers |
| `kernel/arch/<arch>/include/arch/` | What the arch provides to generic code, included as `<arch/...>` |
| `kernel/arch/<arch>/include/arch/<arch>.*.h` | Arch-private headers that generic code must not include |
| `<name>_internal.h` | What the files of one directory share, included as `"<name>_internal.h"` |

Rules:

- A generic header such as `vex/pmap.h` includes `<arch/pmap.h>` and declares
  the arch-neutral interface. Generic code includes the `vex/` header only.
- A userspace header re-exports the `__k_` types from `usr/vex/` as `_t`
  typedefs, for example `typedef struct __k_stat stat_t;`.
- A filesystem has no public header. Other code reaches it through the VFS and
  names its driver with a string.
- A kernel service that needs files of its own has a public header under its
  own name, such as `vex/shmem.h`.

Headers under `include/usr/` must compile as C89. Kernel headers use
`#pragma once` only.

## Generic and arch

`kernel/arch/amd64/` mirrors the generic directories and implements their
arch-specific halves:

```
kernel/mm/            kernel/arch/amd64/mm/
kernel/proc/          kernel/arch/amd64/proc/
kernel/irq/           kernel/arch/amd64/irq/
kernel/time/          kernel/arch/amd64/time/
kernel/firmware/      kernel/arch/amd64/firmware/
kernel/bus/           kernel/arch/amd64/bus/
kernel/core/          kernel/arch/amd64/core/
```

A file that the architecture implements lives in its arch directory. A file
that the architecture only offers to the generic code lives in
`include/arch/`. amd64 is the only implemented architecture. The build knows
the names of riscv64 and aarch64, but there is no code for them.

## Naming and placement

- Every kernel type has a `_t` typedef. The tag ends in `_s`, `_u` or
  `_e`: `struct thread_s`, `union fat_entry_u`, `enum cpu_feature_e`.
- A public function is `<subsystem>_<verb>`: `block_interface_add_root`, not
  `add_block_interface_root`.
- Name a variable after the stem of its type: `thread_t* thread`. A role
  name such as `parent` or `curr` is better when it says more.
- Use whole words in new names. Established terms stay: `vnode`, `pmap`,
  `pslab`, `pdma`, `hart`, `irq`.
- Do not rename an existing file. The short file names will be renamed
  together later.

Placement follows the device model:

- A driver belongs in `drivers/<class>/`.
- A class core, such as the tty layer, belongs in `dev/`.
- A device that shows a kernel subsystem to userspace stays with that
  subsystem: `/dev/hwtree` in `device/`, `/dev/kplog` in `core/`.
- A filesystem belongs in `fs/<name>/` and registers with `vfs_driver_add`.
- Vendored code belongs in `contrib/`, not in `kernel/`.

[CONTRIBUTING.md](https://github.com/vex-os/kernel/blob/main/CONTRIBUTING.md)
holds the full style rules.

## Where to add things

| Task | Touch |
|---|---|
| A driver | A file or directory in `kernel/drivers/<class>/`, a `Kconfig` entry, a Meson gate, `define_driver` |
| A filesystem | `kernel/fs/<name>/`, a `Kconfig` entry, a Meson gate, `vfs_driver_add` |
| A system call | A number in `include/usr/vex/syscall.h`, a handler in `proc/sys_*.c` with `define_syscall*` |
| An initcall | A function with `initcall_bsp_<stage>(fn, INIT_*)` or `initcall_smp_<stage>`, right after its definition |
| A top directory | The directory and its `meson.build`, a row in `kernel_subsystems` |
| A Kconfig option | A `config` block in the directory's `Kconfig`, a gate in its `meson.build` |
| A test | `ktest/test.<name>.c` and a line in `ktest_programs` |
| A font | A `.bin` file in `fbcons/fonts/`, an entry in the `FBCONS_FONT` choice and a default in `FBCONS_FONT_FILE` |

A new system call needs no dispatch table change. The registration macro
places the handler in the `.syscalls` section, and `proc/syscall.c` builds the
table at the syscall stage.
