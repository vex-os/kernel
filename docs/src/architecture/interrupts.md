# Interrupts

A device raises an interrupt when it needs the CPU. The CPU suspends the
running thread and takes a vector from the IDT. One stub saves the registers,
and `irq_dispatch()` calls the handler that a driver registered. The mechanism
turns a hardware signal into a call of a C function.

Three numbers appear on that path, and the page keeps them apart. A *vector*
is an index in the IDT. An *IRQ number* is an index in the handler tables of
`kernel/irq/irq.c`. A *GSI* names an input of an interrupt controller, and the
firmware or the bus gives it once.

The hardware on this page is amd64: the IDT, the 8259 PIC, the IOAPIC and the
LAPIC. A port implements the same `arch_*` entry points for its own controller
model.

## Vectors

The IDT has `IDT_SIZE` entries, 256 in all. Every installed gate points into a
stub in `idts.S`. The CPU pushes an error code for some exceptions only, so a
stub that has none pushes a zero itself.

`arch_init_idt_bsp()` loads the table at `INIT_EARLY`. Every hart runs
`arch_init_idt_smp()` at `INIT_MIDDLE`, and it loads the same table.

```
 device --> IOAPIC RTE or MSI message --> LAPIC --> IDT gate
        --> irq_stub --> amd64_irq_handler --> EOI
        --> irq_dispatch(number) --> irq_handler_t handler
```

The low vectors serve the CPU. Vectors 0x00 to 0x1F are the exceptions, and
`amd64_isr_handler()` turns one into a panic or a signal. The gates of 0x02,
0x08 and 0x12 use an IST from `IDT_IST_NMI`, `IDT_IST_DOUBLE_FAULT` and
`IDT_IST_MACHINE_CHECK`. A stack overflow still finds a stack there.

Vectors 0x20 to 0x2F hold the spurious stubs of the old PIC. The gates from
0x30 to 0xFF use an IRQ stub. Only 0x30 to 0xFE carry an IRQ number:
`ARCH_IRQ_BASE` is 0x30, and `ARCH_IRQ_MAX` is 0xFE. The gate 0xFF is the
LAPIC spurious vector, outside the number space.

`amd64_irq_handler()` sends the EOI before it dispatches. A handler therefore
runs with the interrupt already acknowledged, and an asserted level-triggered
input can raise again during it.

## Handler numbers

`irq_handler_add()` gives a number that means the same on every hart. The tick
and the TLB shootdown use it, because a hart sends an IPI to another hart at
that number. `irq_handler_add_hart()` gives a number on one hart only, and
`interrupt_line_t` uses it.

`irq_table` holds the shared entries, and `hart_tables[]` holds one table per
hart. The first call that needs the table of a hart allocates it. A number is
free only when `irq_table` has no handler for it and no hart uses it:
`is_free_everywhere()`.

Shared numbers come from the top, and per-hart numbers from the bottom. A
higher vector has a higher priority in the LAPIC, so the tick and the IPIs sit
above the devices.

`irq_dispatch()` reads the shared entry first, and the entry of its hart
otherwise (`entry_of()`). It raises `in_flight` before it loads the handler,
and the removal waits for that count to reach zero. A dispatch that finds no
handler logs an unhandled IRQ.

## Lines

The device framework calls `interrupt_attach()` for a GSI. A GSI becomes one
`interrupt_line_t`: one hart, one IRQ number of that hart, and a list of
`interrupt_action_t`. `interrupt_controller_find()` selects the controller
whose `gsi_base` and `gsi_count` cover the GSI, and the attach fails with
`ENODEV` without one.

Two drivers share a line only when both ask for `INTERRUPT_SHARED` and agree
on `trigger` and `polarity` (`may_join()`). Any other case fails with `EBUSY`.

`bind_line()` starts with the boot hart and keeps it unless another online
hart has fewer lines. If that hart cannot take the line, it tries every other
hart. A hart that accepts the line adds one entry with
`irq_handler_add_hart()`, and `hart_lines[]` holds the load of that hart.

`program_line()` writes `irq_number + ARCH_IRQ_BASE` into the controller or
into the MSI message. A new line routes its GSI with the entry masked.
`add_action()` unmasks it after the handler is in the list. The order keeps an
interrupt from reaching a handler that is not there yet.

`interrupt_detach()` removes the action, and `destroy_line()` runs when the
last one goes. The destroy masks the line, gives the number back and frees the
memory.

## The IOAPIC

`arch_init_ioapic()` reads the MADT and makes one `interrupt_controller_t` per
IOAPIC entry. `gsi_count` comes from the version register: the maximum
redirect index plus one. Every redirection entry starts masked.

`ioapic_route()` writes a whole RTE. It sets the vector, the destination LAPIC
id, `level` from `TRIGGER_LEVEL`, `active_low` from `ACTIVE_LOW`, and `masked`.
`ioapic_mask()` and `ioapic_unmask()` change one bit of the entry.

An ISA IRQ does not always reach the IOAPIC at the same GSI.
`interrupt_isa_route()` maps it through the MADT interrupt source overrides,
which `record_override()` records. Without an override the GSI is the IRQ
itself, at `TRIGGER_EDGE` and `ACTIVE_HIGH`.

The IOAPIC controllers enter the device tree at `INIT_LATE`. A driver that
uses their GSIs can then depend on their nodes.

## The PIC

The kernel does not route an interrupt through the PIC. `arch_init_pic_bsp()`
remaps the two chips to 0x20 to 0x2F, masks every input and sends
end-of-interrupt commands to both. `arch_init_pic_smp()` installs
`amd64_spur_20` to `amd64_spur_2F` on those vectors. Their handler only logs,
and it sends no EOI.

## The local APIC

`arch_init_lapic()` reads `MSR_IA32_APIC_BASE`, and it panics when the global
enable bit is clear. It then maps one page of registers with `mmio_map()`. It
sets `LAPIC_SVR_ENABLE` and the spurious vector in `LAPIC_REG_SVR`.

`lapic_write(LAPIC_REG_EOI, 0)` acknowledges an interrupt. `lapic_ipi()`
sends an IPI, and it disables interrupts while it writes `LAPIC_REG_ICR_HI`
and `LAPIC_REG_ICR_LO`. The LAPIC timer puts `irq + ARCH_IRQ_BASE` in its LVT
entry.

The spurious vector 0xFF is above `ARCH_IRQ_MAX`, so no IRQ number maps to
it. `amd64_irq_handler()` returns before the EOI for it. A spurious interrupt
is not in service, so an EOI would end another one.

## Message-signalled interrupts

A message source needs no interrupt controller. `interrupt_attach_message()`
allocates a line with `message_ops` and `source`, masks the source, composes
the message and adds the action.

`arch_msi_compose()` builds the address `0xFEE00000` OR the LAPIC id shifted
left by 12. The data byte is the vector. A PCI function writes those values
through its MSI capability, in `msi_write()`. The write clears
`PCI_MSI_CONTROL_MME`, so the function sends one message. It unmasks vector 0
when the capability supports per-vector masking. It also sets
`PCI_CMD_INT_DISABLE`, so the INTx pin stays quiet.

`pci_attach_irq()` uses MSI when the index is 0 and `pci_get_cap()` finds
`PCI_CAP_MSI`. With no such capability at index 0, the device framework falls
back to the INTx pin of the bus. `msi_mask()` and `msi_unmask()` clear and set
`PCI_MSI_CONTROL_ENABLE`.

## Invariants and traps

- A number has one handler on a hart. A shared handler and a hart handler
  never take the same number, in either order.
- A per-hart number goes back with `irq_handler_remove_hart()`, which asserts
  that the entry exists. `irq_handler_remove()` owns the shared entries.
- `irq_handler_remove()` holds `irq_table_lock` while it waits for
  `in_flight`. A handler that calls `irq_handler_add()` can spin on that lock
  while a removal waits for it to return.
- `irq_handler_add_hart()` can allocate the table of the hart. Its caller must
  have interrupts enabled.
- All handlers run with interrupts disabled. A line handler runs on the hart
  of its line, and `line->lock` serializes the actions of one line.
- A line keeps a raw `controller` pointer. `interrupt_controller_remove()`
  asserts that no line uses the controller, so the caller detaches them first.
- A message line is not in `line_head`, so `find_line()` never returns it.

## Limits

- There is no MSI-X. `PCI_CAP_MSIX` is defined, and nothing reads it. A
  function gets one MSI message at most.
- No trap gate is installed. `idt_set_kernel_gate()` takes the flag, and every
  caller passes `false`.
- The IOAPIC RTEs use the fixed delivery mode only. There is no NMI, SMI or
  `ExtINT` route, and no interrupt remapping.
- The PIC only reports spurious interrupts. Nothing routes an ISA IRQ through
  it, and its vectors never map to an IRQ number.
- A line cannot change its hart. There is no affinity request from userspace,
  and no way to move a line off a busy hart.
- The set of IRQ numbers is fixed by `ARCH_IRQ_BASE` and `ARCH_IRQ_MAX`. The
  tables do not grow, and a full table returns `UINT_MAX`.
- `irq_handler_add_hart()` needs memory for the table of the hart. A hart
  whose first line arrives under low memory loses that line with `ENOMEM`.

## Source

| File | Holds |
| --- | --- |
| [irq/irq.c](https://github.com/vex-os/kernel/blob/main/kernel/irq/irq.c#L24) | the two number tables, `entry_of()` and `irq_dispatch()` |
| [intctl.c](https://github.com/vex-os/kernel/blob/main/kernel/irq/intctl.c#L323) | the lines, the hart binding and the controller registry |
| [irq.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/irq.h#L9) | `irq_handler_t` and the two add calls |
| [intctl.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/intctl.h#L28) | the controller and message ops, and `interrupt_attach()` |
| [idt.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/idt.c#L56) | the 256 gates, the selector and the IST index |
| [idts.S](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/idts.S#L73) | the stubs, the saved frame and the return to ring 3 |
| [isr.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/isr.c#L214) | `amd64_isr_handler()`, the faults and the signals |
| [arch irq.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/irq.c#L218) | the EOI, the spurious vector and the IRQ gates |
| [ioapic.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/ioapic.c#L138) | the MADT parse, the RTEs and the ISA overrides |
| [pic.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/irq/pic.c#L36) | the remap of the two chips and the spurious stubs |
| [lapic.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/core/lapic.c#L89) | the MMIO window, `LAPIC_REG_SVR`, the EOI and the IPIs |
| [msi.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/bus/pci/msi.c#L6) | the MSI address and data words |
| [pci.c](https://github.com/vex-os/kernel/blob/main/kernel/bus/pci/pci.c#L241) | the MSI capability and the INTx fallback |
| [resource.c](https://github.com/vex-os/kernel/blob/main/kernel/device/resource.c#L389) | the attach of a device IRQ by bus or by controller |
