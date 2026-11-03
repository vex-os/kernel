# Firmware and ACPI

The firmware or the code knows which devices exist, where their registers are
and which interrupt they raise. A driver reads that knowledge through
`fwnode_t`, not from an ACPI namespace or from the command line.

A provider fills a firmware node. Vex has two providers: ACPI through uACPI,
and fixed descriptions. The device tree consumes both in the same way.

## The firmware node

`fwnode_t` holds an ops table and an opaque `handle`. The ops say what a
consumer may ask. They cover presence, ids, bus address, CPU id, name, children,
resources, INTx route, dependencies and eject.

The `fwnode_t` is a value with no lifetime of its own. A provider passes it to
a callback on the stack. A consumer that keeps a node copies it, as the device
tree does in `device_add()`. Identity is the pair of `ops` and `handle`, which
`fwnode_is_same()` compares.

```
  ACPI tables / namespace        code, or fixed= on the command line
             |                                  |
          uACPI                          fwnode_fixed_t
             |                                  |
             +------------> fwnode_t <----------+
                               |
                    device tree and driver match
```

`fwnode_root()` returns the node that the devices of the system hang from.
ACPI answers with its predefined System Bus node. Without that node the call
returns `-ENODEV`, and the tree starts with an empty root.

## ACPI through uACPI

`contrib/uacpi` holds an unmodified copy of uACPI. The kernel implements the
`uacpi_kernel_*` API in two files. `kernel/firmware/acpi/uacpi.c` holds memory,
time, semaphores, PCI config access, interrupt install and a work queue.
`kernel/arch/amd64/firmware/acpi/uacpi.c` holds port I/O, the interrupt flag
and the arch spinlock.

The RSDP comes from a Limine request and lands in `rsdp_address`. uACPI gets
it from `uacpi_kernel_get_rsdp()`. `init_acpi_tables()` lends uACPI one page
for early table access at the `mmu` stage. At the `bus` stage, `init_acpi()`
loads and initializes the namespace. It sets the interrupt model through
`_PIC`.

`acpi_node_ops` answers from the uACPI namespace:

- `is_present` accepts a node whose `_STA` reports present and enabled.
- `matches` reads `_HID`, then `_CID`. A `Processor` object matches the id
  `ACPI0007`, which older firmware such as QEMU still uses.
- `get_cpu_id` reads `_MAT` first. Without `_MAT`, it matches the processor
  id against a MADT entry.
- `get_name` writes `acpi:` and the four-character name segment, with the
  trailing underscores removed.
- `for_each_resource` translates `_CRS` into `fwnode_resource_t` entries. A
  memory, port or bus range carries its translation and flags.
- `route_pci_interrupt` reads `_PRT`. A route without a source uses its index
  as the GSI, level-triggered, active-low and shared.
- `for_each_dependency` resolves the name paths of `_DEP`.
- `eject` runs `_EJ0`. On success it reports `_OST`. A busy device gets
  `OST_DEVICE_BUSY` and no `_EJ0`.

The child walk visits `Device` and `Processor` objects only. It includes the
children that the firmware says are absent. This is the `for_each_child`
contract. `fwnode_is_present()` tells an absent child from a present one.

The kernel turns an ACPI interrupt into a device. uACPI asks for a GSI, and
the kernel builds a fixed node named `sci` or `gpe_gsi%u`. Its id is
`ACPI_IRQ`, and it carries one IRQ resource. The `acpi_irq` driver attaches to
the node and calls `device_request_irq()`. The GSI then goes through the
interrupt controller that owns it, like every other device.

ACPI 6.5, 5.2.9 defines the SCI as level-triggered, active-low and shared. A
GPE block interrupt has the same shape.

`_Lxx` and `_Exx` send notifications. `handle_notify()` rescans the tree
around the node, or ejects the node on request. It runs as uACPI work on a
kernel thread, so it queues the tree work that `device_worker` runs. The work
thread runs on the boot hart because some firmware SMI handlers assume it.

## Fixed descriptions

`fwnode_fixed_t` holds a name, an id and a resource table.
`fwnode_init_fixed()` installs `fixed_node_ops`. The node is always present.
Its id matches exactly. It has no children, no dependency and no address.
Eject does nothing.

The name becomes the path component `fixed:<name>`. The id keys
`DRIVER_MATCH_FIRMWARE()`, as `_HID` does for an ACPI device.

A fixed description gives a node to hardware that code knows is there. The
PIT adds one for its ports and its IRQ. An IDE channel builds one for its
command and control ranges, its bus-master range and its IRQ. An ACPI
interrupt node is a fixed description. Its `fwnode_fixed_t` is the first field
of `acpi_irq_trampoline_t`.

A `fixed=` command line option puts a fixed description under an existing
device. `fixed=PNP0501,io:0x2e8+8,irq:3` adds the node under the root. A path
before the id selects another parent, as in `fixed=acpi:PCI0/PNP0501,...`.

`parse_option()` takes the text before the first comma. The part after the
last slash is the id. The part before it is the parent path, and no slash
means the root. The remaining fields are resources: `io:<start>+<len>`,
`mem:<start>+<len>`, `irq:<n>`, `gsi:<n>` and `dma:<n>`.

An `irq:` is an ISA IRQ. It gets `FWNODE_IRQ_ISA` and defaults to
edge-triggered and active-high. A `gsi:` has no ISA flag. A range sets its
translation and flags to zero.

The kernel builds the list once, at `INIT_FIRST` of the `bus` stage. A node
appears when `device_sync_firmware_children()` runs on its parent. A parent
path that never matches produces one warning at `INIT_LAST`, not an error.

## Invariants and traps

- A `fwnode_t` from a callback lives on the stack. Copy it if it must outlive
  the callback.
- The description behind `handle` must outlive every device that holds the
  node. `fwnode_init_fixed()` stores the pointer, not a copy.
- `fwnode_is_same()` compares `ops` and `handle`. Two nodes are the same
  device only when both match.
- A child walk includes absent nodes. A consumer that skips
  `fwnode_is_present()` sees hardware that is not there.
- A failed `_STA` read is not an error to the consumer. The node is simply
  absent.
- The kernel writes the `fixed=` list once and never changes it, so its walk
  takes no lock.
- A notify handler does not change the tree. It queues work for
  `device_worker`.
- An ACPI interrupt node is never removed. uACPI never tears down.

## Limits

- There is no flattened device tree provider. Without a System Bus node, only
  fixed descriptions add devices.
- A `fixed=` id is at most 15 characters. A fixed option holds at most 8
  resources.
- A fixed node has no bus address. A bus that places children by address
  cannot place it, so the node becomes unplaced.
- A `_PRT` link with no interrupt returns `-ENXIO`. The kernel does not pick
  one from `_PRS` or set it through `_SRS`.
- The kernel still reports an extended IRQ with a resource source as a system
  GSI. It does not route the IRQ to another interrupt controller.
- No op exposes `_DSD` or a power resource.
- `uacpi_kernel_uninstall_interrupt_handler()` returns
  `UACPI_STATUS_UNIMPLEMENTED`, because the kernel never tears uACPI down.

## Source

| File | Holds |
| --- | --- |
| [fwnode.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/fwnode.h#L100) | the node, its ops, its resources and `fwnode_is_same` |
| [fixed_fwnode.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/fixed_fwnode.c#L106) | the ops of a fixed description |
| [fixed_option.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/fixed_option.c#L133) | the `fixed=` parser, the list and the lost-path warning |
| [acpi.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/acpi/acpi.c#L30) | the uACPI setup and the namespace load |
| [acpi_fwnode.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/acpi/acpi_fwnode.c#L744) | the ACPI ops, the notify handler and `fwnode_root` |
| [uacpi.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/acpi/uacpi.c#L544) | the uACPI kernel API: memory, time, PCI, interrupts and work |
| [acpi_rsdp.c](https://github.com/vex-os/kernel/blob/main/kernel/firmware/acpi/acpi_rsdp.c#L14) | the RSDP from the Limine request |
| [uacpi.c (amd64)](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/firmware/acpi/uacpi.c#L15) | port I/O, the interrupt flag and the arch spinlock |
| [tree.c](https://github.com/vex-os/kernel/blob/main/kernel/device/tree.c#L322) | the firmware child sync and the `fixed=` hook |
| [uart16550_fw.c](https://github.com/vex-os/kernel/blob/main/kernel/drivers/serial/uart16550/uart16550_fw.c#L8) | a driver that matches firmware ids and uses the node's resources |
