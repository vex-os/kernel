# Kconfig

The configuration is a tree of symbols in the `Kconfig` files. `menuconfig`
shows that tree, so this page does not list the options. It says where they
live and how the build uses them.

## Where the options live

| Menu | File |
|---|---|
| Processes and scheduling | `kernel/proc/Kconfig` |
| Console and logging | `kernel/core/Kconfig`, which sources `kernel/fbcons/Kconfig` |
| Block devices | `kernel/dev/Kconfig` |
| Buses | `kernel/bus/Kconfig` |
| Device drivers | `kernel/drivers/Kconfig`, which sources one file per class |
| Filesystems | `kernel/fs/Kconfig` |
| Debugging | `kernel/Kconfig` |
| Arch capabilities | `kernel/arch/<arch>/Kconfig` |

`kernel/Kconfig` is the root of the tree. It sources the arch file, then one
file per menu.

Each file holds the help text for its options. The help text is the
description of how the option changes the build and the behaviour of the
kernel. `menuconfig` shows it in the help pane.

## Conventions

- A `bool` is on or off. The generated header defines it to 1 when it is on,
  and leaves it undefined when it is off.
- An `int` or a `hex` has a range. A `string` is free text.
- A `choice` picks one symbol out of a group. The symbols in the group share
  one prompt.
- `depends on` hides a symbol until another symbol is on. `select` forces a
  symbol on from another one.
- The default belongs in the `Kconfig` file. A defconfig stores only the
  values that differ from the defaults.
- Every option needs a Meson gate in the directory that it turns on. See
  [Build](../getting-started/build.md#configuration).

## The generated header

`scripts/kconfig/gen_config_h.py` turns `build/.config` into
`build/kernel/generated.config.h`. The build force-includes the header into
every translation unit, so `CONFIG_*` macros are always visible.

## Update the config

```sh
ninja -C build menuconfig      # change the values
ninja -C build savedefconfig   # write build/defconfig
```

Copy `build/defconfig` over `configs/<arch>_defconfig` to make the change the
new default.
