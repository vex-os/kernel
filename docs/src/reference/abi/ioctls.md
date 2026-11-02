# ioctls

An ioctl request is one 32-bit number. It carries the direction of the data,
the size of the argument, the class of the device and the command:

```
 31    30 29            16 15       8 7        0
+--------+----------------+----------+----------+
| dir    | size (14 bit)  | group    | command  |
+--------+----------------+----------+----------+
```

The head of `usr/vex/ioctl.h` fixes that layout with `__k_IOCTL_SET_DIR`,
`__k_IOCTL_SET_SIZE`, `__k_IOCTL_SET_GROUP` and `__k_IOCTL_SET_CMD`. The
helpers `__k_IOCTL_VOID`, `__k_IOCTL_READ`, `__k_IOCTL_WRITE` and
`__k_IOCTL_RDWR` build a request from these parts, and each takes `sizeof()`
of the structure that it passes.

## Direction and size

| Direction | Constant | Data moves |
| --- | --- | --- |
| 0 | `__k_IOCTL_DIR_VOID` | Nothing |
| 1 | `__k_IOCTL_DIR_WRITE` | User to kernel |
| 2 | `__k_IOCTL_DIR_READ` | Kernel to user |
| 3 | `__k_IOCTL_DIR_RDWR` | Both ways |

The size field holds the size of the argument, at most 16383 bytes. A driver
can therefore accept a structure of an older size when the kernel has one more
field. The request of each size stays different.

A request of a group that a file does not know fails with `ENOTTY`.

## The groups

| Group | Header | Device |
| --- | --- | --- |
| `'f'` | `ioctl.h` | Any file, such as `__k_FIONREAD` |
| `'F'` | `fb.h` | The framebuffer |
| `'H'` | `hwtree.h` | The device tree feed |
| `'L'` | `kplog.h` | The kernel log feed |
| `'S'` | `serial.h` | A serial port |
| `'T'` | `termios.h` | A terminal |

`__k_FIONREAD` is the only request that every file takes. It reports a
`__k_int32_t` with the bytes that are ready to read.

The pages of the device classes list the requests of each group:

- [Serial port](serial.md) — `'S'`
- [Terminal](terminal.md) — `'T'`
- [Framebuffer](framebuffer.md) — `'F'`
- [Device feeds](feeds.md) — `'L'` and `'H'`

A new request takes the next free command in its group. A new class of device
takes a free letter.

## Source

- [ioctl.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/ioctl.h#L1) — the layout and the generic request
