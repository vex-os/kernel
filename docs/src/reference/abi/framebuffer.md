# Framebuffer

A framebuffer is a character device that a program maps into its address
space. The device node is a name such as `fbd0`. A mapping of the whole device
gives the pixels, and the ioctls of the `'F'` group read the mode and change
it.

## The mode

`__k_VEXFB_GET_INFO` fills a `struct __k_vexfb_info`:

| Field | Meaning |
| --- | --- |
| `width`, `height` | The size in pixels |
| `pitch` | The bytes from the start of one row to the next |
| `bpp` | The bits of one pixel |
| `red`, `green`, `blue`, `alpha` | A bit offset and a bit length for each channel |
| `size` | The bytes of one whole mapping |
| `palette_size` | The entries of the palette, or 0 for direct color |
| `flags` | `__k_VEXFB_FLAG_FLUSH` when the memory is not scanned out directly |

With a palette, the four channel structures have zero lengths and each pixel
is an index. `__k_VEXFB_GET_PALETTE` and `__k_VEXFB_SET_PALETTE` move a range
of entries, each one a `__k_uint32_t` of the form `0x00RRGGBB`.

The flag `__k_VEXFB_FLAG_FLUSH` means that a store to the memory does not
reach the display on its own. `__k_VEXFB_FLUSH` then sends a list of
rectangles, at most 256 of them.

## The requests

| Request | Argument | Meaning |
| --- | --- | --- |
| `__k_VEXFB_GET_INFO` | `__k_vexfb_info` | Read the mode and the layout |
| `__k_VEXFB_FLUSH` | `__k_vexfb_flush` | Send a list of rectangles to the display |
| `__k_VEXFB_GET_PALETTE` | `__k_vexfb_palette` | Read entries, which the kernel fills |
| `__k_VEXFB_SET_PALETTE` | `__k_vexfb_palette` | Write entries |
| `__k_VEXFB_SET_MODE` | `__k_vexfb_mode` | Change the mode |
| `__k_VEXFB_GET_MODES` | `__k_vexfb_modes` | Read the modes that the display suggests |

`__k_VEXFB_GET_PALETTE` carries the direction of a write, because the
structure itself goes from user to kernel. The kernel then fills the array in
the user memory that the structure points to. A program reads the constant
from the header and does not build it.

`__k_VEXFB_SET_MODE` takes one mode: width, height, bits per pixel and no
flags. It unmaps the memory of the old mode, so a later touch of an old
mapping faults again and gets the new mode. `__k_VEXFB_GET_MODES` takes room
for a list and gives the count back. A mode with `__k_VEXFB_MODE_PREFERRED` is
native to the display.

## The mapping

`mmap` of the device node with `PROT_READ` and `PROT_WRITE`, and `MAP_SHARED`,
maps the pixels at offset 0. The size and the layout come from the info
request, and the mapping has the size of one screen.

## Limits

- A list of rectangles holds at most 256 entries.
- A palette entry is 32 bits, `0x00RRGGBB`, with no alpha.
- There is no request for the cursor, the blanking or the display power.
- A mode change invalidates every mapping of the device.

## Source

- [fb.h](https://github.com/vex-os/kernel/blob/main/kernel/include/usr/vex/fb.h#L1) — the structures and the `'F'` requests
- [limine.c](https://github.com/vex-os/kernel/blob/main/kernel/drivers/display/limine.c) — the early framebuffer
- [bochs.c](https://github.com/vex-os/kernel/blob/main/kernel/drivers/display/bochs.c) — the display that can change its mode
