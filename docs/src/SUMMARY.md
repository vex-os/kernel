# Summary

[Introduction](index.md)

# Getting started

- [Build](getting-started/build.md)
- [Run](getting-started/run.md)
- [Source map](getting-started/source-map.md)

# Architecture

- [Overview](architecture/overview.md)
- [Boot and init](architecture/boot.md)
- [Memory](architecture/memory/index.md)
  - [Address types](architecture/memory/addresses.md)
  - [Physical memory](architecture/memory/physical.md)
  - [Virtual memory](architecture/memory/virtual.md)
  - [Commit accounting](architecture/memory/commit.md)
- [Processes](architecture/processes/index.md)
  - [Scheduler](architecture/processes/scheduler.md)
  - [Threads](architecture/processes/threads.md)
  - [Signals](architecture/processes/signals.md)
- [System calls](architecture/syscalls.md)
- [VFS](architecture/vfs.md)
- [Device model](architecture/devices.md)
- [Interrupts](architecture/interrupts.md)
- [Time](architecture/time.md)
- [Firmware and ACPI](architecture/firmware.md)
- [IPC](architecture/ipc.md)
- [Security](architecture/security.md)

# Drivers

- [Overview](drivers/index.md)
- [Storage](drivers/storage/index.md)
  - [AHCI](drivers/storage/ahci.md)
  - [PATA](drivers/storage/pata.md)
- [Display](drivers/display/index.md)
  - [Limine framebuffer](drivers/display/limine.md)
  - [Bochs VBE](drivers/display/bochs.md)
  - [Framebuffer console](drivers/display/fbcons.md)
- [Input](drivers/input/index.md)
  - [PS/2 keyboard](drivers/input/ps2kbd.md)
- [Serial](drivers/serial/index.md)
  - [UART 16550](drivers/serial/uart16550.md)

# Filesystems

- [Overview](filesystems/index.md)
- [ext2](filesystems/ext2.md)
- [FAT](filesystems/fat.md)
- [echfs](filesystems/echfs.md)
- [devfs](filesystems/devfs.md)
- [tmpfs](filesystems/tmpfs.md)

# Buses

- [Overview](buses/index.md)
- [PCI](buses/pci.md)
- [ISA](buses/isa.md)
- [IDE](buses/ide.md)

# Reference

- [Syscalls](reference/syscalls.md)
- [ABI](reference/abi.md)
- [Capabilities](reference/capabilities.md)
- [Device names](reference/device-names.md)
- [Logging](reference/logging.md)
- [Kconfig](reference/kconfig.md)

# Development

- [Testing](development/testing.md)
- [Writing docs](development/writing-docs.md)
