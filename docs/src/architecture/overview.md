# Overview

Vex is one kernel binary. Limine jumps to `kmain`, the kernel prepares its
parts through initcalls, and then it creates the first process. This page
gives the layer map of the source tree and follows the path from boot to
process 1.

The kernel has many parts, and no one function can prepare them in a safe
order. An initcall places each preparation in a linker section, and one runner
walks the sections. The code declares no layer object and no dependency
graph. The map below groups the top directories by their work. The initcall
stage order is the only boot order that the code states.

## The layer map

| Layer | Top directories | What the layer provides |
| --- | --- | --- |
| Boot | `init/` | `kmain`, `mount_root`, `spawn_init` and the runner |
| Execution | `proc/`, `exec/`, `ipc/` | processes, threads, system calls, ELF64, pipes |
| Files | `fs/` | the VFS and the devfs, ext2, FAT, echfs and tmpfs backends |
| Devices | `device/`, `drivers/`, `dev/`, `fbcons/` | the device tree, drivers, interfaces and consoles |
| Buses | `bus/`, `firmware/` | PCI, ISA, IDE and ACPI |
| Core | `core/`, `irq/`, `time/` | logging, interrupts and clocks |
| Entropy | `random/`, `crypto/` | the random pool, its sources and three primitives |
| Memory | `mm/` | `pdma`, `pslab`, the slab allocator, `pmap`, `kmalloc`, `vms` |
| Machine | `arch/amd64/`, `libk/` | CPU tables, paging and the freestanding C support |

`arch/amd64/` is not only the bottom layer. It mirrors `mm/`, `proc/`, `irq/`,
`time/`, `firmware/`, `bus/`, `core/` and `random/`, and supplies the machine
half of each. This map gives that machine half no entry of its own.

amd64 is the only architecture with code today. The build maps the riscv64
and aarch64 names, and the EDK2 and Limine binaries for both are vendored.
A port adds `kernel/arch/<arch>/` and implements the machine half of each
mirrored directory. The generic half does not change.

`kernel/include/` has no archive. It holds the headers that all layers include.

A directory is not a stage. One directory can add initcalls at several
stages: `random/` registers at `early`, `bootinfo`, `services` and `dev`. The
stage of a call, not the directory, gives the boot order.

Each enabled entry in `kernel_subsystems` builds one static archive. The table
gives each archive one `KP_SUBSYSTEM` tag, so its `pr_*` calls share a log
prefix. `ld` links the archives into `vexsys.elf` with `--whole-archive`,
which keeps an object that holds only a registration.

## The path from boot to the first process

The linker script names `kmain` in `ENTRY(kmain)`. Limine loads `vexsys.elf`
and jumps to `kmain`. The kernel is position-independent, and it reads its
real base from `limine_executable_address_request`.

`kmain` then does five things:

1. It prints the version banner.
2. It calls `invoke_initcalls(initcalls_bsp_start, initcalls_bsp_end)`.
3. It mounts the filesystem from the `root=` device at `/`.
4. It loads `init=` or `CONFIG_DEFAULT_INIT` as the first process.
5. It enables interrupts and spins in `hart_spin()`.

Interrupts are off for steps 1 to 4, and the scheduler does not switch yet.

An initcall is a `{ .function, .name }` entry in a linker section. The macro
`initcall_bsp_<stage>(fn, order)` writes it to
`.initcalls.bsp.<stage number><order>`. The linker script brackets the BSP
sections with `initcalls_bsp_start` and `initcalls_bsp_end`, and `SORT` puts
them in name order. The stage number is the prefix in the section name. The
order is one of `INIT_FIRST`, `INIT_EARLY`, `INIT_MIDDLE`, `INIT_LATE` or
`INIT_LAST`.

The stages run in this order: `early`, `arch`, `bootinfo`, `pmm`, `slab`,
`mmu`, `heap`, `irq`, `timer`, `syscall`, `multicore`, `bus`, `vfs`, `dev`,
`fs`, `cpudev`, `binfmt`, `services`, `late`.

`mount_root` reads `root=` with `cmdline_argument` and finds the interface with
`block_interface_find`. It calls `vfs_mount` for `/` with a null type. The VFS
then probes each registered driver that has an `ops->probe`, and stops at the
first that accepts the volume.

`spawn_init` calls `process_create(nullptr)`. `next_pid` starts at `INIT_PID`,
which is 1, so the process gets PID 1. It holds a reference to
`kernel_credentials`, and its working directory is the VFS root. `spawn_init`
then opens the program, loads it, and starts the user thread and the
`init_watchdog` kernel thread.

`load_binary` finds the ELF64 loader because `init_elf64` added it to the
registry at the `binfmt` stage. `thread_spawn_*` only enqueues the thread. The
first scheduler interrupt after `enable_interrupts()` switches to the init
thread. The kernel does not return to the `kmain` stack.

The boot order is the dependency order. The `dev` stage adds the block
interface and parses its partition table before `kmain` calls `mount_root`. The
`vfs` and `fs` stages register the filesystem drivers before `vfs_mount` probes
them. The `binfmt` stage registers the loader before `load_binary` asks for it.

## Invariants and traps

- **The stage is the dependency.** An initcall uses only what an earlier
  stage prepared. `slab_alloc` before `slab`, or `vfs_*` before `vfs`, is a bug.
- **The order is data, not a call list.** No code runs the functions in
  sequence. The link must keep an object that no symbol references, and
  `--whole-archive` does.
- **Within one stage and order, link order decides.** Do not rely on it. Use a
  different stage or order.
- **Interrupts are off in a BSP initcall.** Do not block, sleep or take a lock
  that can block.
- **`bsp_` runs once, `smp_` runs on every hart.** The boot hart also runs the
  SMP stream, from `init_hart_boot` at `multicore`/`INIT_LAST`.
- **An initcall cannot report failure.** It returns `void`. `invoke_initcalls`
  has no error path, so an initcall calls `panic()`.
- **`spawn_init` starts one user process and one kernel thread.** `init` is
  PID 1. `init_watchdog` is not a process.
- **`init` must not exit.** `init_watchdog` waits on `init_exited`, calls
  `vfs_sync_all()` and panics.

## Limits

- The code has no declared dependency graph between directories.
  `kernel_subsystems` is a flat list of archives and log tags. Nothing stops
  `fs/` from including a header of `proc/`, or a lower layer from calling a
  higher one. Real files cross the layers. `core/kplog.c` uses the char
  interface of `dev/` and `file_t` of `fs/`. `dev/tty_interface.c` uses
  `process_t` and the scheduler.
- `arch/amd64/` is not a bottom layer. It mirrors eight generic directories,
  and its code interleaves them, so a strict stack is a simplification.
- Only amd64 has code. The build maps the riscv64 and aarch64 names, but
  `kernel/arch/` has no code for them.
- `cpudev` is a stage with no call. The order list is complete, but the stage
  is empty today.
- `kmain` starts no shell, service manager or second user process. Everything
  after process 1 is the job of `init`.
- `init_hart_boot` has no timeout. A hart that never reports online hangs the
  boot.
- The layer map describes the source tree, not a runtime boundary. Every layer
  runs in ring 0 with the same heap and the same direct map.

## Source

| File | What it holds |
| --- | --- |
| [kernel/meson.build](https://github.com/vex-os/kernel/blob/main/kernel/meson.build#L65) | The `kernel_subsystems` table and the `--whole-archive` link |
| [kernel/arch/amd64/kernel.ld](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/kernel.ld#L3) | `ENTRY(kmain)` and the `SORT` of the initcall sections |
| [kernel/include/vex/initcall.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/initcall.h#L25) | The stage macros and the order constants |
| [kernel/init/initcall.c](https://github.com/vex-os/kernel/blob/main/kernel/init/initcall.c#L4) | The runner that walks a start and end pair |
| [kernel/init/kmain.c](https://github.com/vex-os/kernel/blob/main/kernel/init/kmain.c#L105) | `kmain`, `mount_root`, `spawn_init` and `init_watchdog` |
| [kernel/proc/process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/process.c#L300) | `process_create`, the PID counter and `init_process_root` |
| [kernel/proc/sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c#L838) | `thread_spawn_user` and `thread_spawn_kernel` |
| [kernel/fs/mount.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/mount.c#L419) | `vfs_mount` and the probe that picks a filesystem driver |
| [kernel/exec/binfmt.c](https://github.com/vex-os/kernel/blob/main/kernel/exec/binfmt.c#L89) | `load_binary` and the registry walk |
| [kernel/exec/elf64.c](https://github.com/vex-os/kernel/blob/main/kernel/exec/elf64.c#L480) | `init_elf64`, the ELF64 loader for the `binfmt` stage |
