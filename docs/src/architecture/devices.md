# Device model

Vex must drive hardware that firmware describes and hardware that code finds.
`kernel/device/` holds one tree of `device_t` nodes for this. A bus adds and
removes the nodes that it finds. A driver claims a node and publishes an
interface on top, which devfs shows to userspace.

After init, the tree changes only on `device_worker`, one kernel thread.
Probe, attach and detach run there. A driver can sleep during attach, and the
tree needs no lock around driver code.

## The device tree

`device_get_root()` makes the root on the first call, from `fwnode_root()` when
the firmware describes one. The root has id 0 and the name `""`. Every other
node takes the next value of `next_id`, so no id repeats. `device_add()` adds
a child below a parent and gives the caller a new reference to it.

All children of a parent share one list through `priv.next` and
`priv.children`. `add_node()` walks that list and returns `-EEXIST` for a
name that a sibling already has. A name is unique among siblings only, not in
the tree. `device_get_path()` writes the path from the root, such as
`acpi:PCI0/pci:0000:00:1f.0/isa:3f8`.

A child holds a reference to its parent. The tree holds one reference to each
node, and `device_add()` gives the caller another. `device_decref()` frees a
node at its last reference. It asserts that its children, interrupts, mappings,
providers, consumers and `waits_for` are all empty. `remove_node()` takes
the node out of the list and then drops the tree's reference. A node that
others still hold stays alive.

`copy_resources()` copies the resources of the node's firmware node. It skips
a range with `FWNODE_RANGE_WINDOW`, which a bridge passes on to its children.
The copy holds at most `DEVICE_RESOURCE_MAX` (16) entries.

`device_sync_firmware_children()` compares the firmware children with the
tree. It places a present child through `bus_ops->place_child()`. When that op
returns `-ENOENT` or is missing, it adds the child as an unplaced node. It
removes an absent child only when `firmware_judges()` is true. The test holds
for an unplaced child. It also holds when the parent is not a bus, or has no
`rescan` op. A bus with `rescan` judges its own children.

`device_bus_of()` returns `nullptr` for an unplaced child, even when the parent
is a bus. Such a child uses its firmware description for ranges and interrupts,
and the parent bus ignores it.

`device_set_gone()` marks the node and its subtree as gone.
`device_is_gone()` is true for that flag, or for the state `DEVICE_GONE`. A
bus that lost the hardware calls it before it queues the removal.

## device_worker

Every change after init goes through the worker:

```
   device_queue_request(node, request)
        |  pending |= request, one reference, enqueue once
        v
   queue_head --> device_worker
                     |  queue_pop, then run_queue
                     v
   EJECT > REMOVE > UNBIND, then RESCAN
                     |
                     v
   bind -> wait -> attach -> rescan children
```

`device_queue_request()` accepts any context. It sets the bit of the request
in `priv.pending` under `queue_lock`, puts the node in a FIFO once, takes a
reference, and triggers `worker_wakeup`. A second request for a queued node
only sets another bit.

`run_queue()` pops the node and takes all pending bits. It acts in this order:

1. `DEVICE_REQUEST_EJECT` runs `eject_node()`, which calls `fwnode_eject()`.
2. `DEVICE_REQUEST_REMOVE` runs `take_offline()`, then `remove_node()`.
3. `DEVICE_REQUEST_UNBIND` runs `take_offline()`, then `unbind_node()`.
4. `DEVICE_REQUEST_RESCAN` runs `rescan_node()`.

A removal or an eject makes the other requests of the node moot. An unbind
comes before a rescan, so a node asked for both ends up bound anew.
`take_offline()` calls `offline` on every child and consumer first, and on
the node last. If any of them refuses, `online_node()` restores all of them
and the request does not happen.

`init_device_tree()` runs at `INIT_LATE`. It warns about each driver that
`is_complete()` rejects, and makes the root. The scheduler does not run yet,
so it rescans the root on the spot and then starts the worker.

`/dev/hwtree` reports each change. Its ioctls queue a rescan, an unbind or an
eject for a node id. Userspace never changes the tree directly.

## Probe and attach

`is_complete()` accepts a driver only with `detach` and `release`, and with
both `offline` and `online` or neither. `init_device_tree()` warns about the
other drivers and leaves them out.

`bind()` walks the `drivers_start` to `drivers_end` range, which the
`.drivers` section fills through `define_driver`. For each driver it takes the
first `driver_match_t` that matches and calls `probe()`. The probe returns a
`DRIVER_SCORE_*` level, a higher value, or `-ENODEV`. It never returns zero.

The device goes to the driver with the highest score. A tie goes to the driver
that comes first in the section. `attach()` then runs with no tree lock, and
may sleep.

On success the node gets `driver` and the state `DEVICE_BOUND`.
`device_wake_waiters()` then queues a rescan of every node that waited for it.

On failure:

- `attach` returning `-EAGAIN` with a dependency sets `DEVICE_WAITING`. The
  node attaches later, when `device_wake_waiters()` queues its rescan.
- Any other error sets `DEVICE_PRESENT` and `attach_failed`. A rescan does not
  bind the node again.
- `undo_attach()` runs `device_release_resources()` and `release()`, and
  clears `ctxdata`.
- The device stays unbound. It does not fall back to a lower-scored driver.

`device_wait_for_dependencies()` walks `fwnode_for_each_dependency()`, which
ACPI's `_DEP` fills. `device_wait_for()` links the consumer to a bound
provider, so `unbind_consumers()` unbinds the consumer first. It returns
`-EDEADLK` for a cycle and `-ENODEV` for a provider that is gone. A provider
that is not bound yet gives `-EAGAIN`. `device_request_irq()` calls
`device_use()` for the controller of the GSI.

## Interfaces

A node is not a file. A driver publishes a `block_interface_t` with
`block_interface_add_root()`, or a `char_interface_t` with
`char_interface_add()`. The interface has a name from `dev_name_t`, an ops
table and a reference count. Its `device` pointer names the node, and the
driver sets it before the add.

[devfs](../filesystems/devfs.md) subscribes to the block and char event
buses and mirrors each interface as a vnode. A block interface with
`BLOCK_FLAG_HIDDEN` gets no vnode. It forwards the vnode operations to the
interface, so userspace reaches a device only through that interface. The
interface name follows [Device names](../reference/device-names.md).

`char_interface_t` serves a byte stream: `open`, `read`, `write`, `seek`,
`ioctl`, `poll`, `mmap` and `resolve_page`. `block_interface_t` serves
sectors through the `submit` op. A driver for a terminal line, a serial port
or a framebuffer embeds a `char_interface_t`. The containing type is
`tty_interface_t`, `serial_interface_t` or `fb_interface_t`, and the ops
belong to that core.

`device_request_irq()` and `device_map_io()` record what
`device_release_resources()` takes back. That happens after the driver detaches
the device, or after its `attach` fails. The framework does not remove an
interface. `detach()` removes it, and `release()` frees the state.

## Invariants and traps

- `device_t` state changes only on `device_worker`.
- `device_tree_mutex` guards `children`, `next`, the provider and consumer
  links, and the interrupt and mapping lists. No driver call runs under it.
- A child holds a reference to its parent. A node cannot outlive its children.
- A failed `attach` is final for the node: a rescan does not bind it again. An
  unbind clears `attach_failed`.
- An `offline` refusal cancels the whole request. `online_node()` undoes every
  `offline` that succeeded.
- A resource list holds 16 entries. `collect_resource()` drops the 17th entry
  without an error.
- `device_bus_of()` ignores the bus of an unplaced node. A child that
  `bus_ops->place_child()` rejected gets no bus translation.
- `remove_node()` unbinds the root but leaves it in the tree.

## Limits

- There is one worker thread. A slow `attach` delays every other node.
- After a failed `attach`, the node waits for an unbind, and no other driver
  gets the device.
- The tree has no device class, no power management beyond `offline` and
  `online`, and no user-space driver.
- Hotplug comes only from a rescan. A bus finds its hardware, a firmware
  notify starts one, or userspace asks for one.
- Sibling names are unique. Names at different depths need not be.
- A name that does not fit `DEVICE_NAME_MAX` (32) bytes truncates silently.
- A path that does not fit `DEVICE_PATH_MAX` (256) truncates silently.
- An interface is the driver's to remove in `detach()`. The framework never
  removes one, so a driver that skips it leaks the interface.

## Source

| File | Holds |
| --- | --- |
| [tree.c](https://github.com/vex-os/kernel/blob/main/kernel/device/tree.c#L137) | the root, the child list, `device_add`, resources and the reference count |
| [bind.c](https://github.com/vex-os/kernel/blob/main/kernel/device/bind.c#L448) | the worker, the request queue, the score and the attach result |
| [depend.c](https://github.com/vex-os/kernel/blob/main/kernel/device/depend.c#L89) | the provider links and the wait for a provider |
| [resource.c](https://github.com/vex-os/kernel/blob/main/kernel/device/resource.c#L209) | ranges, mappings, interrupts and `device_release_resources` |
| [device_internal.h](https://github.com/vex-os/kernel/blob/main/kernel/device/device_internal.h#L80) | the request bits, `device_link_t` and `device_bus_of` |
| [device.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/device.h#L18) | `device_t`, the state enum and the resource API |
| [device_bus.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/device_bus.h#L18) | `bus_ops_t` and the range translation |
| [driver.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/driver.h#L10) | `driver_t`, `driver_match_t`, the scores and `define_driver` |
| [fwnode.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/fwnode.h#L51) | the firmware node, its ops and its resources |
| [hwtree.c](https://github.com/vex-os/kernel/blob/main/kernel/device/hwtree.c#L134) | the userspace view of the tree and its requests |
| [char_interface.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/char_interface.h#L79) | the char interface, its ops and the registry |
| [char_interface.c](https://github.com/vex-os/kernel/blob/main/kernel/dev/char_interface.c#L17) | the char registry and the open count |
| [block_interface.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/block_interface.h#L60) | the block interface, its ops and the request |
