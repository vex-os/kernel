# Kernel plan by subsystem

Status: agreed, active. Base: `main` at `6312e19`, 2026-11-03.

Scope: the kernel only. The mlibc work is in
[mlibc-sysdeps-plan.md](mlibc-sysdeps-plan.md).

This plan replaces the kernel part of `syscall-plan.md` and
`syscall-undone.md` in the bootstrap repository. The system call ABI from
those documents is complete. This plan starts from the code.

## How to use this plan

- One item is one commit. A large item may be a short series of commits with
  one subject.
- Cost: `S` is about one day, `M` is about one week, `L` is a new subsystem
  or a change in several subsystems.
- Status: `now` is the current wave. `next` is the following wave. `later`
  waits for a user or for hardware. `parked` is a decision against the work.
- Done means three things. A ktest passes. The architecture chapter matches
  the code. This plan loses the item.
- Keep the item IDs stable. A commit subject may name its item, for example
  `proc: run a woken thread before the current one (proc-1)`.
- A parked item is a decision. Do not leave a `TODO` for a parked item in the
  code. Write the reason in the chapter or here.
- The subsystems are directories, not layers. The code has no declared
  dependency graph, and a real file crosses the directories.

## State of the tree

The kernel boots on amd64 from Limine over UEFI and runs on several harts.
The system call ABI covers the POSIX calls that mlibc needs. The set is
processes, threads, signals, memory, descriptors, paths, mounts, time,
random numbers, polling, job control, spawn, CPU time and resource limits.

The rest of the work is mostly not ABI work. It is scheduler policy, a clean
shutdown, hardware coverage, and the documented limits of each subsystem.
The sections below group the work by directory.

| Area | State | Main gap |
| --- | --- | --- |
| Boot and init | Limine, initcalls, init as process 1 | init does not sync and unmount, the shutdown numbers are only reserved |
| Processes | fork, exec, spawn, wait, job control, itimers, CPU time | no ptrace |
| Scheduling | `SCHED_OTHER`, nice changes the slice, affinity | nice is not a priority, no load balance |
| Signals | full set, queue, alt stack, `sigtimedwait` | `SA_NOCLDWAIT` ignored, no core file |
| Memory | pmm, slab, pmap, vms, strict commit, shmem | no swap, no `RLIMIT_AS`, partial `mlockall` |
| VFS | vnode, vcache, mount, path, file, locks | device node outside devfs, one rename lock |
| Filesystems | devfs, tmpfs, ext2, FAT, echfs | ext2 device nodes, FAT Unicode names |
| Devices | device tree, one worker, block and char, tty | no pty, no NVMe, no virtio, one worker |
| Buses | PCI, ISA, IDE | bridge bus numbers |
| Interrupts | LAPIC, IOAPIC, MSI, PIC fallback | no MSI-X, no line affinity |
| Time | clock sources, HPET and TSC, wall clock | no broadcast, no suspend, no tickless idle |
| IPC | pipes, futex, shmem, `ppoll` | no local sockets, no robust futex, no memfd |
| Random | chacha20, blake2s, sources, devices | no virtio-rng, no SHA-256, no `random=` |
| Firmware | uACPI, fixed descriptions | no FDT, no `_PRS` or `_SRS`, extended IRQ |
| Arch | amd64 only | no other arch, no x2APIC, a CPU device is matched by LAPIC ID |
| Docs | 20 stub pages | the old plans were out of date |

## Start here

1. `boot-1`, make the test init sync and unmount before it ends the run. The
   item is in the bootstrap repository. The kernel keeps the panic.
2. `boot-2`, reserve the two shutdown numbers. The handlers come later.
3. `proc-1`, run a woken thread before the current thread. It makes nice a
   priority and closes the last two scheduler items of the old plan.
4. `mm-2`, finish `mlockall`. It is small and closes the last `UNDONE` of the
   memory subsystem.
5. `test-2`, so that the shutdown test has a place in the run order.
6. `doc-1` and `doc-2` continue in the background. Write a page when the
   code of its subsystem changes.
7. The mlibc session starts in parallel. It needs no new kernel call.

## Boot and init

**State.** `kmain()` runs the BSP initcalls, mounts `root=`, starts `init=`
and idles. Init is process 1. When init dies, the kernel panics. This is the
intended behavior, because init lives as long as the kernel. The panic path
syncs the filesystems first, as a test aid.

**Limits we keep.** The kernel panics when init dies. Init owns everything
after process 1. There is no shell, service manager or second user process in
the kernel. There is no declared dependency graph between the directories.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| boot-1 | Make the test init sync and unmount before it ends the run (bootstrap repository) | S | now |
| boot-2 | Reserve `SYS_REBOOT` and `SYS_SHUTDOWN` in the ABI | S | now |
| boot-3 | Implement `SYS_REBOOT` and `SYS_SHUTDOWN` | M | later |
| boot-4 | Time out a hart that never reports online | S | later |
| boot-5 | Give the empty `cpudev` stage its calls, or drop the stage | S | later |
| boot-6 | Declare the dependency graph between directories | L | parked |

**Notes.**

- **boot-1.** Init is process 1, and process 1 never ends on a UNIX system.
  The kernel leans on that: it hands the orphans of a dying process to init,
  and init owns the job control tree. An absent process 1 is a crash, so the
  panic is the correct answer and stays
  ([kmain.c](https://github.com/vex-os/kernel/blob/main/kernel/init/kmain.c#L49)).
  What changes is the test init, `kst`. It is the init of the test image, so it
  syncs and unmounts every mount except `/` before it ends the run, as a system
  manager does before it asks for a power off. The item lives in the bootstrap
  repository, so it is one commit there, not in the kernel. The
  `vfs_sync_all` in the panic path stays: it makes an image that a broken test
  left behind readable, and that is a different case from a clean end of the
  run. Done when a test image whose `kst` unmounts before it ends the run
  passes `e2fsck -fn`, and the panic path still syncs.
- **boot-2.** Reserve two numbers in the system block of
  `usr/vex/syscall.h`: `REBOOT` at `0x12B` and `SHUTDOWN` at `0x12C`. Write
  no handler. The dispatcher gives `ENOSYS` for an empty slot. This fixes the
  numbers before a program depends on them.
- **boot-3.** Add the handlers. Both need `CAP_SYS_ADMIN`. Both sync the
  filesystems first and do not return on success. A restart uses the ACPI
  reset register. A power off uses `_S5` or the fixed feature register. If
  ACPI is absent, a restart uses a triple fault. The handler neither stops the
  services nor unmounts: the caller does that first and then asks for the power
  off, as a system manager does before a shutdown call. Depends on `boot-2`.
  mlibc does not need the calls.
- **boot-4.** `init_hart_boot` has no timeout. A hart that never reports
  online hangs the boot. The protocol parks each AP until the kernel writes
  `goto_address` with a release store. The AP then runs on a bootloader stack
  and gets its `limine_mp_info` in `rdi`
  ([hart.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/hart.c#L20)).
  `probed_harts` counts the APs that reached `hart_park`, and the boot waits
  for that count without a deadline
  ([hart.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/hart.c#L133)).
  Give that wait a deadline from the clock source, then continue with the
  harts that arrived. The kernel sends no stack size request, so the
  bootloader default applies. Add `limine_stack_size_request` if the probe
  needs more room.
- **boot-5.** The `cpudev` stage exists in the order and has no call. Either
  add the calls that belong there, such as the CPU driver and the early
  device tree, or remove the stage from the order.

## Processes and scheduling

**State.** `fork`, `exec`, `spawn`, `waitpid`, `waitid`, process groups,
sessions, job control, interval timers, CPU time and resource limits work.
The scheduler has one FIFO run queue per hart and steals work only when a
hart has nothing to run. `nice` belongs to the process and changes only the
length of its timeslice.

**Limits we keep.** `SCHED_OTHER` with priority 0 is the only policy until
`proc-3`. Nice belongs to the process, not to the thread. Affinity is the
only user control of placement. `MAX_HARTS` is a build constant.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| proc-1 | Run a woken thread before the current thread, and order the queue by nice | M | now |
| proc-2 | Balance run queues on the tick | M | next |
| proc-3 | `SCHED_FIFO` and `SCHED_RR` | M | later |
| proc-4 | Take a hart offline and move its work | L | later |
| proc-5 | Decide the next size class for `process_t` | S | later |
| proc-6 | `ptrace` and tracer stops | L | parked |

**Notes.**

- **proc-1.** `choose_hart` already puts a woken thread into the queue of a
  hart that may run it, even when that hart is busy
  ([sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c#L249)).
  The tick handler already switches to a queued thread from a self-IPI
  ([sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c#L336)).
  Three changes close the gap.
  - `rq_take_nolock` takes the head of the FIFO. It must take the best
    thread by nice value. A scan is enough, because the queue is short.
  - The tick handler must keep the current thread when the current thread
    has a better nice value than the best queued thread.
  - `kick_hart` must send an IPI to a busy hart when the woken thread is
    better than the thread that runs there.

  Done when a ktest starts a thread with a better nice value and sees it
  run before the current thread uses its full timeslice. The same test
  checks that equal nice values keep round-robin order.- **proc-2.** The `TODO` in `choose_hart` records the limit. A busy hart
  keeps its queue. Add a periodic check on the tick, behind a Kconfig
  option. Move a thread to a hart that may run it and has less work. Do not
  move a thread with `bound_hart`. Done when a ktest with more threads than
  harts sees an even spread.
- **proc-3.** `sys_setscheduler` refuses every policy but `SCHED_OTHER`
  ([sys_sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_sched.c#L273)).
  Add a policy field and a priority to the thread. A FIFO thread runs until
  it blocks or a better thread wakes. An RR thread gets a slice. Priority
  changes need `CAP_SYS_NICE`. Depends on `proc-1`.
- **proc-4.** `cpu_offline` refuses every hart, including the boot hart
  ([cpu.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/core/cpu.c#L46)).
  A real offline moves the threads and the timers to other harts and removes
  the hart from the scheduler. The boot hart stays until its duties can
  move. Depends on `proc-2` and `time-1`.
- **proc-5.** `process_t` and `thread_t` have a budget of 512 bytes
  ([process.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/process.c#L31),
  [sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c#L39)),
  because the slab allocates them. Every slab page starts with a header of one
  block that holds the slab pointer, so a class gives up one block per page
  ([slab.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/slab.c#L56)):
  a 1024 byte block fits 3 objects on a page, 512 fits 7, and 256 fits 15. The
  budget keeps the object in the class that loses the least. A new field
  either goes into a shared object, as the credentials do, or the type moves
  to the 1024 byte class and loses a quarter of every page. Decide when a
  field is needed.
- **proc-6.** The stop machinery is ready: `THREAD_WORK_STOP`, `job_park`,
  the wait status of a stop, and `WUNTRACED`. A tracer needs a syscall, a
  register interface, a memory access path, and a policy for `SIGSTOP`
  against a traced child. Design it before you write it. Depends on `mm-3`.

## Signals

**State.** `kill`, `tgkill`, `sigaction`, `sigprocmask`, `sigpending`,
`sigaltstack`, `sigreturn`, `sigsuspend`, `sigtimedwait`, `sigqueue` and
`setitimer` work. The kernel passes `(sig, siginfo *, ucontext *)` to every
handler, and the frame keeps the old mask. Stop and continue signals drive
job control.

**Limits we keep.** `SA_SIGINFO` is accepted and not read. The three
argument call is compatible with a one argument handler on amd64. `SA_NOCLDSTOP`
is read. `SA_NOCLDWAIT` is not. `DEFAULT_CORE` ends the process and
writes no core file. A real-time signal can lose a value when memory is
short.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| sig-1 | Honor `SA_NOCLDWAIT` | S | later |
| sig-2 | Write a core file for `DEFAULT_CORE` | M | parked |
| sig-3 | Tracer stops and `CLD_TRAPPED` | L | parked |

**Notes.**

- **sig-1.** A process that sets `SA_NOCLDWAIT` must not leave zombies. Mark
  the child as reaped at exit when the flag is set, and keep the stop
  report. A shell sets the flag. mlibc does not need it.
- **sig-2 and sig-3** belong to a debugger and a crash reporter. Do not
  start them before a user exists.

## Memory

**State.** `pdma` and `pslab` supply frames. A slab and `kmalloc` serve
small objects. `pmap` holds 3, 4 or 5 level page tables. `vms` holds the
regions, `rmap` holds the reverse maps, and `commit` holds the strict commit
limit. Shared anonymous memory is a file on an internal tmpfs. There is no
swap. The file cache reclaims clean pages.

**Limits we keep.** A user page is 4 KiB, never huge. There is no swap, no
overcommit mode, no OOM killer and no page table reclaim. Device memory
takes no commit charge. `mlock` counts bytes and pins nothing.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| mm-1 | Give `MADV_WILLNEED` a readahead | S to M | next |
| mm-2 | Apply `MCL_CURRENT` and `MCL_FUTURE` in `mlockall` | S to M | now |
| mm-3 | Reach device memory of another address space | M | later |
| mm-4 | Enforce `RLIMIT_AS` | S | later |
| mm-5 | Huge pages for user memory | L | parked |
| mm-6 | Swap and an OOM killer | L | parked |
| mm-7 | Reclaim page tables | M | parked |
| mm-8 | NUMA zones and memory hotplug | L | parked |

**Notes.**

- **mm-1.** `vms_populate` faults a range in, one page at a time
  ([vms.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/vms.c#L1403)).
  `MAP_POPULATE` uses it for the whole range, anonymous regions included, and
  that stays. `MADV_WILLNEED` promises something else: it says that a program
  is about to touch the pages, not that the kernel must map them now. An
  anonymous page has no medium to read from, so only the file-backed regions
  of the range can gain. The cheapest form calls it for those regions and maps
  the pages into the process. A cache-only form needs a new vnode operation and
  a wrapper in ext2, FAT and echfs. The fcache part is small, because
  `get_page` and `fill_page` already exist. The cache now
  evicts clean pages, so readahead cannot leak memory
  ([fcache.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/fcache.c#L994)).
  Decide the form first. mlibc does not need the call.
- **mm-2.** `locked_bytes` and `RLIMIT_MEMLOCK` work
  ([sys_mman.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L361)).
  `mlockall` checks the flags and does nothing
  ([sys_mman.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_mman.c#L471)).
  For `MCL_CURRENT`, walk the regions and charge their sizes. For
  `MCL_FUTURE`, add a `locked` bit to `vm_region_t`, as `noreserve` is.
  `alloc_region` and `free_region` charge and uncharge. A fork copies the
  total. With no swap, the pages are counted, not pinned. Done when a ktest
  checks the limits and the `ENOMEM` path.
- **mm-3.** `access_begin_locked` refuses device memory of another address
  space, because the mapping is not loaded
  ([vms.c](https://github.com/vex-os/kernel/blob/main/kernel/mm/vms.c#L519)).
  A debugger and a core file need the path. It needs a temporary kernel
  mapping and the consent of the driver, because a read of an MMIO register
  may have side effects.
- **mm-4.** The commit code enforces its own limit. `RLIMIT_AS` limits one
  process and is stored only. Check the total size of the regions in
  `vms_map`, `vms_map_any`, `mremap` and the stack growth. Return `ENOMEM`.
- **mm-5 to mm-8** are parked. A user or hardware must ask for them first.

## VFS and filesystems

**State.** The VFS has vnodes, a vcache, mounts, path resolution, files and
range locks. devfs, tmpfs, ext2, FAT and echfs serve it. The page cache
writes back dirty pages, and the writeback thread runs by itself. `O_TMPFILE`
and unnamed files work.

**Limits we keep.** One mount tree, and a process root is the only
per-process view. No lazy unmount. No name cache. One global rename mutex,
and one global vcache unused list. tmpfs takes one mutex for all operations.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| fs-1 | Find an interface by `rdev`, and add the `nodev` option | S to M | later |
| fs-2 | Device nodes on ext2 | M | parked |
| fs-3 | One rename lock per filesystem | S | later |
| fs-4 | Lazy unmount | M | later |
| fs-5 | One vcache unused list per filesystem | S | later |
| fs-6 | A name cache in the VFS | L | parked |
| fs-7 | Unicode long names on FAT | M | later |
| fs-8 | A hash table for tmpfs directories | S to M | later |
| fs-9 | Mount namespaces | L | parked |

**Notes.**

- **fs-1.** `device_node_open` returns `ENXIO`
  ([vnode.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/vnode.c#L215)).
  The char and block private structures carry `dev`
  ([char_interface.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/char_interface.h#L71)),
  and both registries expose `for_each`. Add a lookup by `dev` and use it in
  the open path. The I/O code is the one of `devfs_char_ops` and
  `devfs_block_ops`, moved to a shared place. Add the `nodev` mount option:
  a mount with `nodev` fails an open of a device node with `ENXIO`. The
  lookup is small. The delegation of every operation is the larger part.
  Done when `mknod` on tmpfs makes a node that opens, and `nodev` blocks the
  same node.
- **fs-2.** `ext2_vnode.c` marks device nodes as `UNDONE`, because a `dev_t`
  takes 64 bits and an inode has 32 for it
  ([ext2_vnode.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/ext2/ext2_vnode.c#L970)).
  Either store the Linux 32 bit encoding of major and minor, or extend the
  inode. No user needs it.
- **fs-3.** One mutex covers every rename in the kernel. Move it into `vfs_t`.
  The VFS serializes each filesystem already.
- **fs-4.** A busy filesystem stays mounted until its last user goes. Add a
  detached list and a mount that no path leads to, as Linux does.
- **fs-7.** FAT long names keep the low byte of a UTF-16 code unit
  ([fat_dirent.c](https://github.com/vex-os/kernel/blob/main/kernel/fs/fat/fat_dirent.c#L171)).
  Convert the name to UTF-8 for `getdents`, and back on create.
- **fs-8.** A tmpfs directory lookup walks a list. Add a hash table when a
  large directory is slow.

## Devices, drivers and buses

**State.** One device tree holds every node and one worker thread changes
it. Drivers declare themselves with `define_driver`. The block and char
registries feed devfs and the tty layer. The tree has AHCI, PATA, the
Limine framebuffer, the Bochs VBE display, the PS/2 keyboard and a 16550
UART. PCI, ISA and IDE are the buses. The tty discipline sits in `dev/`.

**Limits we keep.** One worker thread. No device class and no power
management beyond `offline` and `online`. No user-space driver. Sibling
names are unique. An interface is the driver's to remove.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| drv-1 | A modern virtio-PCI transport | L | next |
| drv-2 | virtio-blk as a block interface | M | next |
| drv-3 | virtio-rng as a random source | S | next |
| drv-4 | NVMe driver | L | later |
| drv-5 | MSI-X and multi-vector MSI | M | later |
| drv-6 | Move an IRQ line to another hart | S to M | later |
| drv-7 | Assign bus numbers under a PCI bridge | S | later |
| drv-8 | Set the PATA transfer modes | S | later |
| drv-9 | UART flow control and DCD | S | later |
| drv-10 | Bochs VBE: old std-vga and own modes | M | parked |
| drv-11 | Remove the interfaces that a boot claim added | S | later |
| drv-12 | pty: `/dev/ptmx` and `/dev/pts` | L | parked |
| drv-13 | Device classes and power management | L | parked |
| drv-14 | A second device worker | S | later |
| drv-15 | xHCI and USB storage and input | L | parked |

**Notes.**

- **drv-1 to drv-3.** The kernel has no virtio code. The device name table
  reserves a name for virtio block
  ([device-names.md](https://github.com/vex-os/kernel/blob/main/docs/src/reference/device-names.md#L54)).
  Plan a modern virtio-PCI device (version 2) with the common
  configuration structure. Then add virtio-blk as a block interface, and
  virtio-rng as a polled random source. QEMU then boots the test image
  without AHCI, and WHPX gets entropy without RDSEED. Legacy virtio is not
  planned. Depends on `drv-5` for good throughput, but not for a first
  version.
- **drv-4.** NVMe needs the MSI-X path and a queue pair model. It gives a
  modern machine a disk.
- **drv-5.** `PCI_CAP_MSIX` is defined and nothing reads it. A function
  gets one MSI message at most. The PCI code must program the table, and the
  IRQ layer must give a vector per queue.
- **drv-6.** A line cannot change its hart. The IOAPIC redirection entry and
  the vector table hold the target. Add an interface for a driver and for
  `hwtree`.
- **drv-7.** `pci.c` records a bridge without bus numbers as `UNDONE`
  ([pci.c](https://github.com/vex-os/kernel/blob/main/kernel/bus/pci/pci.c#L432)).
  Assign the numbers and program the bridge windows.
- **drv-8.** `pata.c` keeps the firmware transfer modes
  ([pata.c](https://github.com/vex-os/kernel/blob/main/kernel/drivers/storage/pata.c#L941)).
  Set the highest mode that both sides report.
- **drv-9.** The UART has no `SERIAL_FLOW_RTSCTS` and no DCD
  ([uart16550.c](https://github.com/vex-os/kernel/blob/main/kernel/drivers/serial/uart16550/uart16550.c#L183)).
  A modem consumer needs both.
- **drv-11.** `fb_boot_claim` refuses a second claim, because a removal
  would leave a gap in the names
  ([fb_interface.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/fb_interface.c#L563)).
  Remove the added interfaces and their names instead.
- **drv-12, drv-13 and drv-15** are parked. A pty belongs to a terminal
  multiplexer on a KMS console. A device class belongs to a power-managed
  machine. USB belongs to real hardware support.

## Interrupts, timers and time

**State.** The LAPIC and IOAPIC deliver interrupts. MSI works with one
message. The PIC reports spurious interrupts only. Clock sources are the
HPET, the TSC and the PIT. Each hart has a timer, and the scheduler tick
uses a oneshot. The wall clock steps and never slews. CPU time comes from a
cycle counter where the CPU allows it.

**Limits we keep.** No interrupt remapping, no NMI route, no `ExtINT`
route. A trap gate is not installed. The tick has the resolution of the
clock source. The RTC is UTC, with no zone.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| time-1 | Broadcast a global timer to idle harts | M | later |
| time-2 | Switch the clock source before an offline | S | later |
| time-3 | `CLOCK_BOOTTIME` when suspend exists | S | parked |
| time-4 | Suspend and resume | L | parked |
| time-5 | Tickless idle | M | parked |
| time-6 | A trap gate for kernel debugging | S | later |
| time-7 | Take the TSC frequency from Limine | S | next |

**Notes.**

- **time-1.** A global timer serves the boot hart alone, and a broadcast is
  `UNDONE`
  ([clockevt.c](https://github.com/vex-os/kernel/blob/main/kernel/time/clockevt.c#L19)).
  A hart whose local timer stops in idle loses its tick. Add a broadcast or
  a per-hart fallback timer for idle. `time-5` needs it.
- **time-2.** `hpet_offline` refuses to remove the active source
  ([hpet.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/time/hpet.c#L86)).
  Switch to another source first.
- **time-3.** The realtime and monotonic clocks alias the raw and coarse
  clocks, because the kernel steps and never slews
  ([sys_time.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_time.c#L20)).
  `CLOCK_BOOTTIME` must count the time in suspend after `time-4` exists.
- **time-6.** `idt_set_kernel_gate` takes a trap flag and every caller
  passes `false`. A trap gate keeps interrupts on for a breakpoint and a
  debug fault.
- **time-7.** The protocol has a TSC frequency request
  (`limine_tsc_frequency_request`). The kernel does not send it, so CPU time
  takes the frequency from CPUID or from the calibration. The bootloader
  value is best effort. Prefer it when a response arrives, and keep the
  current checks. Done when a machine with a nonzero response needs no
  calibration round, and the 1% warning against CPUID stays quiet.

## IPC

**State.** Pipes and FIFOs work, with a 64 KiB queue. Futexes work with
private and shared keys. Shared anonymous memory is a file on an internal
tmpfs. `ppoll` works with a generation counter, not a waiter list.

**Limits we keep.** No priority inheritance, no robust list, no requeue, no
waitv. No `splice`, no packet mode, no `memfd_create` and no sealing. No
System V shared memory. No local sockets yet.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| ipc-1 | `AF_UNIX` sockets | L | later |
| ipc-2 | Robust futexes | M | next |
| ipc-3 | `memfd_create` and sealing | M | later |
| ipc-4 | `FUTEX_WAITV` and `FUTEX_REQUEUE` | S | parked |
| ipc-5 | `splice` and packet mode | M | parked |

**Notes.**

- **ipc-1.** `kernel/ipc/` holds the pipe service, and local sockets belong
  there too. Start with `SOCK_STREAM` over the pipe code, then
  `SOCK_DGRAM`. Decide the address rules, the credential passing and the
  file descriptor passing before you write the code. No user needs it
  before networking.
- **ipc-2.** mlibc may implement robust mutexes. The kernel needs the robust
  list at thread exit, `FUTEX_ROBUST` state and `ENOTRECOVERABLE`. Check
  what mlibc expects first. A blocked thread that dies must wake its
  waiters with `EOWNERDEAD`.
- **ipc-3.** A `memfd` is a tmpfs file with no name and a seal set. The
  `tmpfile` operation already makes the file. Seals need a vnode field and
  checks in `write`, `truncate` and `fallocate`.

## Random numbers

**State.** The generator is chacha20, and the pool uses blake2s. Sources
are RDSEED, RDRAND, jitter, IRQ timing, and Limine. `GETRANDOM` and the two
devices work. The random thread folds the event buffers into the pool.

**Limits we keep.** No Fortuna pools. No generator for users to choose. No
claim of entropy that the kernel cannot prove. A trusted source that lies
can seed the generator with weak data.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| rnd-1 | SHA-256 | S | later |
| rnd-2 | MT19937-64 as the duty generator | S | parked |
| rnd-3 | The `random=` command line option | S | later |
| rnd-4 | 32 Fortuna pools | L | parked |
| rnd-5 | A floppy noise source | S | parked |

**Notes.**

- **rnd-1.** The plan for item 21 of the syscall plan asked for SHA-256 for
  Fortuna. One pool and blake2s serve today. Add SHA-256 when another
  subsystem needs it.
- **rnd-2.** MT19937-64 needs 2.5 KiB per hart and serves only test code and
  tie breaks. Keep it parked until a caller exists.
- **rnd-3.** `random.trust_<name>` already reads the command line with
  `cmdline_argument`
  ([random_source.c](https://github.com/vex-os/kernel/blob/main/kernel/random/random_source.c#L110)).
  Add `random=<generator>` in the same place. A registry of generators comes
  with the second generator.
- **rnd-5.** An empty floppy drive may give noisy reads. Give it low credit
  and log a notice. This is not priority work.

## Firmware and ACPI

**State.** uACPI provides the namespace. ACPI devices become fwnodes and
then device nodes. Fixed descriptions add a device from the command line.
The kernel has no flattened device tree provider.

**Limits we keep.** No FDT until a non-x86 arch exists. `_DSD` and power
resources are not exposed. A fixed node has no bus address.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| fw-1 | Pick an interrupt from `_PRS` and set it with `_SRS` | M | later |
| fw-2 | Route an extended IRQ with a resource source | M | later |
| fw-3 | A flattened device tree provider | L | parked |
| fw-4 | `_DSD` and power resources | M | later |

**Notes.**

- **fw-1.** A `_PRT` link with no interrupt returns `-ENXIO`
  ([acpi_fwnode.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/acpi/acpi_fwnode.c#L576)).
  The kernel does not read `_PRS` or write `_SRS`.
- **fw-3.** The protocol has a device tree request (`limine_dtb_request`).
  Use it when the machine has a DTB and no ACPI. The provider then reads one
  format, not two.
- **fw-2.** An extended IRQ with a resource source is reported as a system
  GSI
  ([acpi_fwnode.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/acpi/acpi_fwnode.c#L462)).
  The IRQ layer has one controller per line today.

## Arch

**State.** amd64 is the only architecture with code. The pmap detects 3, 4
or 5 level paging. CPU time uses `rdtsc` when the CPU has a constant rate.
The MP request sends no flags, so Limine turns x2APIC off. A CPU device
finds its hart by the LAPIC ID. The other architecture names map in the
build, but no code exists.

**Limits we keep.** No 32-bit user address space. No hart hotplug until
`proc-4`.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| arch-1 | A riscv64 port | L | parked |
| arch-2 | An aarch64 port | L | parked |
| arch-3 | x2APIC mode, and ask Limine for it | M | later |
| arch-4 | Match a CPU device by ACPI UID, not LAPIC ID | S | later |

**Notes.**

- **arch-1 and arch-2.** Limit the work to the interfaces that
  `<arch/*.h>` names. Add a hart bring-up, a pmap, a clock source, an
  interrupt controller and a random source. Do not start before the amd64
  code stops changing.
- **arch-3.** The MP request sets no flags
  ([hart.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/proc/hart.c#L12)).
  The protocol says that Limine then tries to turn x2APIC off, and fails to
  boot if it cannot. Set `LIMINE_MP_REQUEST_X86_64_X2APIC` and program the
  x2APIC MSRs for IPIs, the timer and the local APIC ID. The response flag
  says whether the mode is on. A 32-bit LAPIC ID is not enough for a large
  machine, and the boot must not fail on firmware that already turned the
  mode on.
- **arch-4.** `find_hart` compares the ACPI UID of the CPU device with the
  LAPIC ID ([cpu.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/core/cpu.c#L15)).
  The protocol gives `processor_id` (the ACPI UID) and `lapic_id` as
  separate fields. Store both in `hart_local_t`, match the device by the
  UID, and use the LAPIC ID only for IPIs. The two values usually agree, so
  this bug appears only on some firmware.

## Tests and docs

**State.** `ktest` boots the test image, and `kst` runs the tests. The
architecture book has 20 stub pages. The old plans went out of date, because
no rule tied them to the code.

**Limits we keep.** A test returns 0 and uses `ktest_expect`. A test that
takes over the machine runs last.

**Items.**

| ID | Work | Cost | Status |
| --- | --- | --- | --- |
| test-1 | Give every item of this plan a ktest | S each | now |
| test-2 | Let a shutdown test run after the framebuffer test | S | now |
| doc-1 | Write the 20 stub pages | S each | now |
| doc-2 | Update the chapter in the same commit as the code | S each | now |

**Notes.**

- **test-2.** The framebuffer test goes last today, because it takes over
  the screen. A shutdown test takes over the machine. Add it after the
  framebuffer test in `ktest/meson.build`, and document the order. The test
  itself comes with `boot-3`.
- **doc-1.** The empty pages are `drivers/**`, `buses/**` and
  `filesystems/{index,ext2,fat,echfs}.md`. A page needs a
  `## Why it is done this way` section. Merge a small driver into the table
  of its section instead of a page.
- **doc-2.** A commit that changes a behavior of this plan updates the
  architecture chapter and this plan together. The old plans failed this
  rule.

## Order of work

**Wave 1, now.** `boot-1`, `boot-2`, `proc-1`, `mm-2`, `test-2`. Start the
mlibc session at the same time. These items make the test loop clean and make
nice a priority. No item blocks mlibc.

**Wave 2, next.** `proc-2`, `mm-1`, `ipc-2`, `drv-1`, `drv-2`, `drv-3` and
`time-7`. The wave has the queue balance, the readahead, the robust futex
path, the first virtio support and the TSC frequency from the bootloader.

**Wave 3, later.** `boot-3`, `boot-4`, `boot-5`, `proc-3`, `proc-4`,
`proc-5`, `sig-1`, `mm-3`, `mm-4`, `fs-1`, `fs-3`, `fs-4`, `fs-5`, `fs-7`,
`fs-8`, `drv-4` to `drv-9`, `drv-11`, `drv-14`, `time-1`, `time-2`, `time-6`,
`ipc-1`, `ipc-3`, `rnd-1`, `rnd-3`, `fw-1`, `fw-2`, `fw-4`, `arch-3` and
`arch-4`.

**Wave 4, when asked.** The parked items. A parked item needs a user, a
machine or a specification before it starts.

The docs work (`doc-1`, `doc-2`) runs with every wave.

## Not planned

- Swap, an OOM killer and an overcommit mode. Strict commit accounting makes
  them unnecessary for the current use.
- Huge pages, NUMA, memory hotplug and page table reclaim. They serve
  machines that this kernel does not target yet.
- A device class, device power management and user-space drivers.
- An LSM, ACLs and a policy for setuid transitions beyond the credentials
  code.
- Mount namespaces and a VFS name cache.
- `SCHED_BATCH` and `SCHED_IDLE`.
- `splice`, pipe packet mode, `FUTEX_WAITV` and `FUTEX_REQUEUE`.
- Fortuna pools, MT19937-64 as a user source, and a user choice of
  generator.
- 32-bit user code and a second architecture before the amd64 code stops
  changing.
- An IOMMU driver. Limine base revision 5 turns Intel VT-d and AMD-Vi off
  unless the kernel asks to keep them. A driver would first request
  `limine_x86_64_keep_iommu_request`.

## Decisions

- A woken thread preempts a running thread with a worse nice value. The
  scheduler keeps one queue per hart and scans it. It does not build
  priority queues until a measurement says otherwise.
- The kernel panics when init dies, by design. Process 1 never ends on a
  UNIX system, and the kernel leans on it. The test init syncs and unmounts
  before it ends the run. A shutdown call is a system call, and its numbers
  are reserved before the handlers exist.
- A device node outside devfs opens through a shared device I/O path. devfs
  stays the place that names devices.
- The virtio transport is the modern version only. Legacy virtio is not
  planned.
- Readahead for `MADV_WILLNEED` starts with the file-backed regions. A
  cache-only path comes when a program has a need.
- `mlock` and `mlockall` count bytes. With no swap, "locked" is a promise
  about the commit limit, not about frames.
- The kernel asks for Limine base revision 6. The MP request sends no flags
  until `arch-3`, so x2APIC stays off.
- A CPU device matches its hart by the ACPI UID, not by the LAPIC ID. The
  protocol gives them as separate fields.
- `SYS_PREADV`, `SYS_PWRITEV` and `SYS_SPAWN` have no caller in mlibc. Their
  ABI is the kernel's own and not the Linux one, so each gets a header of its
  own under `include/usr/vex/` when a user appears. mlibc runs `posix_spawn`
  on `fork` and `exec` today.

## Not verified

- The cost estimates. Only `proc-1`, `boot-1` and `mm-2` were studied in
  detail.
- The ext2, FAT and echfs internals beyond the `UNDONE` lines.
- The ACPI reset and power-off path with uACPI. `boot-3` needs a probe on
  QEMU first.
- The source of the init program in bootstrap. `boot-1` assumes that it can
  call `sync` and `umount`.
- The Limine protocol text (base revision 6) was read for the MP, stack
  size, TSC frequency, device tree, entropy and IOMMU features. The kernel
  side of `arch-3`, `arch-4`, `boot-4`, `time-7` and `fw-3` was checked
  against it. The x2APIC code path was not read.
- Whether a cache-only readahead fits `fcache` without a new lock.
- The virtio device versions that QEMU offers with the current machine
  types.
- The permission rules for a `nodev` mount and the capability that a mount
  needs. The VFS has `CAP_MOUNT`.

## Maintenance

- Add an item before you write the code. Keep the item ID in the commit
  subject.
- When an item lands, remove it here and update the chapter. A plan that
  describes the past is worse than no plan.
- A new limit in the code gets a line in the `## Limits` section of its
  chapter.
- Do not copy this plan into the book. The book describes the code. This
  plan describes work.
