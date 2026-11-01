# Vex

Vex is a hobby operating system kernel written in C (gnu2x) and built with
Meson. Limine boots it over UEFI. amd64 is the only implemented architecture:
the Meson build knows the riscv64 and aarch64 names, but there is no
`kernel/arch/` code for them yet.

This book is the architecture guide. It explains how the kernel is put together
and why each part is done the way it is. It is not an API reference: the
declarations in `kernel/include/` carry their own `///` documentation, and
clangd shows it on hover.

## Where to start

- [Build](getting-started/build.md) the kernel and the test image.
- [Run](getting-started/run.md) it in QEMU.
- [Source map](getting-started/source-map.md) to find where something lives.
- [Overview](architecture/overview.md) for the whole picture.

## How this book is organised

- **Getting started** covers the build, the test image and the source tree.
- **Architecture** describes the kernel-wide mechanisms: boot, memory,
  processes, system calls, the VFS, the device model.
- **Drivers**, **Filesystems** and **Buses** hold one page per implementation.
  A page exists when the implementation makes a decision worth explaining.
- **Reference** holds tables: syscalls, the ABI, capabilities, device names.
- **Development** covers testing and how to update this book.

## Status

The getting-started pages are complete. Most architecture pages are still
stubs. Until they are complete, `docs/kernel/architecture.md` describes the
kernel-wide mechanisms, and `CONTRIBUTING.md` holds the full style rules.
