# Boot and init

Vex boots in three steps. Limine loads the kernel and jumps to `kmain`. The boot hart runs the BSP initcalls with interrupts off. `kmain` then mounts the root filesystem and starts `init`. This page follows that path. The full contract of the first process is in [Run](../getting-started/run.md#the-first-process).

## The handoff

Limine loads `vexsys.elf`, builds the page tables, and jumps to `kmain` (`ENTRY(kmain)` in the linker script). The kernel is linked at `0xFFFFFFFF80000000` and is position-independent, so Limine may slide it. The kernel reads the real bases from `limine_executable_address_request`. It does not assume the link address.

Limine gives the kernel a stack of at least 64 KiB, clears `IF`, and loads a GDT. The IDT is undefined, so the kernel loads its own. The kernel places its requests in `.limine_requests` with base revision 6. They cover the HHDM, memory map, MP table, executable address, command line and RSDP. They also cover the framebuffer, entropy, boot date and paging mode.

The [Limine boot protocol](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md) defines the handoff, the machine state and each request.

![The boot flow](boot-flow.svg)

## The BSP initcalls

`kmain()` calls `invoke_initcalls(initcalls_bsp_start, initcalls_bsp_end)`. Interrupts are off, and the scheduler does not run yet. The calls run in the order that the linker script gives them.

A subsystem declares a call with `initcall_bsp_<stage>(fn, order)`:

```c
static void init_ext2(void)
{
    ...
}

initcall_bsp_vfs(init_ext2, INIT_EARLY);
```

The macro writes a `{ .function = fn, .name = "fn" }` entry into `.initcalls.bsp.<stage><order>`. For `init_ext2`, that is `.initcalls.bsp.3100000.3`. The stage is the suffix of the macro name, `vfs`. The macro gives the stage the number `3100000` in the section name. The order is `INIT_FIRST`, `EARLY`, `MIDDLE`, `LATE` or `LAST`. The linker script brackets the BSP sections with `initcalls_bsp_start` and `initcalls_bsp_end`, and it sorts them with `SORT`.

![How an initcall becomes a call](initcalls.svg)

### The stages

| Stage | Prefix | What it prepares | Examples |
| --- | --- | --- | --- |
| `early` | `0000000` | The boot thread and the earliest output | `arch_init_boot_thread`, `arch_init_bxcons`, `init_random` |
| `arch` | `0100000` | CPU tables and the APs | `init_harts`, `arch_init_gdt_bsp`, `arch_init_idt_bsp`, `arch_init_isr` |
| `bootinfo` | `0200000` | What Limine reported | `init_kbase`, `init_hhdm`, `init_memmap`, `init_cmdline_parse` |
| `pmm` | `1000000` | Physical page allocators | `init_pdma`, `init_pframe`, `init_pslab`, `init_commit` |
| `slab` | `1100000` | The slab allocator | `init_slab` |
| `mmu` | `1200000` | The kernel page tables | `init_acpi_tables`, `init_tlb`, `init_mmu` |
| `heap` | `1300000` | The kernel heap and stacks | `init_liminefb`, `init_kstack`, `init_cmdline_preserve` |
| `irq` | `2000000` | The interrupt controllers | `arch_init_ioapic` |
| `timer` | `2100000` | Clock sources and events | `arch_init_hpet`, `init_acpi_pmt`, `init_cycles`, `init_wallclock` |
| `syscall` | `2200000` | The dispatch table | `init_syscall` |
| `multicore` | `2300000` | Idle threads and the AP release | `report_cpucaps`, `sched_init`, `init_acpi_work`, `init_hart_boot` |
| `bus` | `3000000` | PCI, ISA and the ACPI nodes | `arch_init_pci_CF8`, `init_fixed_options`, `init_acpi` |
| `vfs` | `3100000` | The VFS and its drivers | `init_process_root`, `init_devfs`, `init_ext2`, `init_fat`, `init_tmpfs` |
| `dev` | `3200000` | Devices and their interfaces | `init_tty`, `init_block_ptable`, `init_kplog`, `init_device_tree`, `init_fb_boot` |
| `fs` | `3300000` | Filesystems that need devices | `init_echfs`, `init_pipefs`, `init_shmem` |
| `cpudev` | `3400000` | Per-CPU devices | No calls today |
| `binfmt` | `3500000` | Executable formats | `init_elf64` |
| `services` | `4000000` | Kernel threads for services | `init_random_worker` |
| `late` | `ff00000` | The last BSP work | `sched_start`, `report_lost_options` |

The stage and the order are the sort key. A call may use what the earlier stages prepared. This order is the dependency system. Within one stage and order, the link order sets the sequence. Do not rely on it. `invoke_initcalls` does not look at a return value. An initcall returns `void` and calls `panic()` on failure.

A `bsp_` call runs once, on the boot hart. An `smp_` call runs on every hart that comes online, the boot hart included. The SMP stages are `hart`, `arch`, `irq`, `syscall`, `timer` and `late`. `bring_online` runs them.

## The APs

`init_harts` at `arch`/`INIT_FIRST` reads the Limine MP response. It registers each hart. It calls `arch_start_hart` to give the AP an entry point through `goto_address`. The AP enters `hart_park`, probes its CPU features, and waits.

The boot hart does not wait. It continues through the stages. At `multicore`/`INIT_LAST`, `init_hart_boot` sets the bring-up state to `GO`. It then waits until every usable hart reports online.

Each AP then runs `run_idle`:

1. Switch to `sys_pmap`.
2. Call `bring_online`.
3. Enable interrupts.
4. Halt in `hart_stop` until the scheduler wakes it.

`bring_online` calls `hart_setup`, runs the `initcalls_smp_*` stream, marks the TLB as online, and sets `HART_FLAG_ONLINE`. On amd64, the SMP stream initializes the GDT and TSS, the IDT, the PAT and the FPU. It also initializes the PIC, the LAPIC, the syscall entry, and the LAPIC timer on each hart.

The boot hart runs the same `bring_online` path inside `init_hart_boot`. After that call, its thread pointer is the boot hart's idle thread. The static boot thread from `arch_init_boot_thread` is no longer current.

## The first process

After the BSP initcalls, `kmain()` runs the last steps:

1. It prints the version banner.
2. It mounts the block device from `root=` at `/`.
3. It opens the path from `init=`, or from `CONFIG_DEFAULT_INIT`.
4. It loads the ELF through the binfmt registry and creates the process.
5. It spawns the user thread and the `init_watchdog` kernel thread.
6. It enables interrupts and spins.

The order matters. The `dev` stage attached the block driver and parsed the partition tables, so `root=` names a device. The `binfmt` stage registered the ELF loader, so `load_binary` finds it. The `vfs` stage registered the root source and the filesystem drivers, so `vfs_mount` can mount the root.

The process is PID 1. It has the kernel credentials, `/` as its working directory, no open file descriptors, and an empty environment. [Run](../getting-started/run.md#the-first-process) has the full contract.

`init_watchdog` waits on `init_exited`. When init dies, the watchdog syncs the filesystems and panics. Init must not exit.

`sched_start` at `late`/`INIT_FIRST` already sent the scheduler IPI to every hart. The boot hart has interrupts off, so its IPI stays pending until step 6. The first scheduler interrupt runs `sched_irq_handler`. That handler updates the clock, wakes expired waits, runs expired timers, and switches to the init thread. The kernel does not return to the `kmain` stack.

## Invariants and traps

- **The stage declares dependencies.** A call may use only what earlier stages prepared. `kmalloc` before `heap`, or `vfs_*` before `vfs`, is a bug. The bug may survive until the first use.
- **Interrupts are off.** Do not block, sleep, or take a lock that can block in a BSP initcall. A blocking call cannot work. The scheduler switch needs interrupts enabled.
- **The scheduler does not run yet.** `thread_spawn_*` only enqueues. `sched_start` starts the ticks.
- **`bsp_` once, `smp_` on every hart.** The boot hart runs the SMP stream too, at `multicore`/`INIT_LAST`.
- **Within one stage and one order, the link order sets the sequence.** Do not rely on it. Use a different stage or order.
- **The runner ignores return values.** An initcall returns `void`. Panic on failure.
- **`init_hart_boot` has no timeout.** A hart that never reports online hangs the boot.
- **The command line points into bootloader memory.** `init_cmdline_preserve` copies it at `heap`/`INIT_LAST`. Use `cmdline_argument` after that point.
- **The page tables change at `mmu`.** Before `init_mmu`, the kernel runs on Limine's tables. After it, the kernel runs on `sys_pmap`. The kernel maps the HHDM in the same step.
- **`cpudev` is empty.** The stage exists. No code uses it today.
- **The link must include the whole archive.** A registration is data that no code references. The link uses `--whole-archive`. Without it, the linker drops the object and the initcall.

## Where the code is

| File | Contents |
| --- | --- |
| [kmain.c](https://github.com/vex-os/kernel/blob/main/kernel/init/kmain.c) | Entry point, `root=` mount, `init=` load, watchdog |
| [initcall.c](https://github.com/vex-os/kernel/blob/main/kernel/init/initcall.c) | The runner |
| [initcall.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/initcall.h) | The macros, stages and orders |
| [kernel.ld](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/kernel.ld) | Entry point, sections, start and end symbols |
| [hart.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/hart.c) | Hart registration, bring-up, SMP stream |
| [hart.c (amd64)](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/hart.c) | MP request, AP entry, GS and TSS |
| [sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c) | Idle threads, `sched_start`, the tick |
| [process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/process.c) | `process_create`, the init root source |
| [elf64.c](https://github.com/vex-os/kernel/blob/main/kernel/exec/elf64.c) | The ELF64 loader |
