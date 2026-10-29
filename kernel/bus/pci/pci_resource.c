#include <vex/device.h>
#include <vex/kprintf.h>
#include <vex/pci.h>

#include "pci_internal.h"

#define PCI_BUS_WINDOW_MAX (PCI_ROOT_WINDOW_MAX + 3)

#define IO_SPACE_START    UINT64_C(0x1000)      // ISA devices and the PCI configuration ports are below
#define IO_SPACE_END      UINT64_C(0x10000)     // PCI Local Bus 3.0, 6.2.5.1: I/O BARs decode 16 bits on x86
#define BELOW_4G_END      UINT64_C(0x100000000) // A 32-bit BAR's reach
#define ADDRESS_SPACE_END UINT64_MAX

enum space_decode_e {
    SPACE_UNUSED, // The function has no BARs in the space, and its decoding is left as it is
    SPACE_OFF,
    SPACE_ON,
};

typedef enum space_decode_e space_decode_t;

typedef struct pci_window_s pci_window_t;
typedef struct pci_window_list_s pci_window_list_t;

struct pci_window_s {
    bool is_io;
    bool prefetchable; // Only a prefetchable BAR may go in it
    uint64_t start;
    uint64_t end; // Exclusive
};

struct pci_window_list_s {
    pci_window_t windows[PCI_BUS_WINDOW_MAX];
    size_t num_windows;
};

static void add_window(pci_window_list_t* restrict list, bool is_io, bool prefetchable, uint64_t start, uint64_t end)
{
    if(start >= end || list->num_windows == PCI_BUS_WINDOW_MAX) {
        return;
    }

    pci_window_t* window = &list->windows[list->num_windows];
    window->is_io = is_io;
    window->prefetchable = prefetchable;
    window->start = start;
    window->end = end;

    list->num_windows += 1;
}

// PCI-to-PCI Bridge Architecture 1.2, 3.2.5.6-3.2.5.10: a window
// whose base is above its limit is off; limits are inclusive
static void read_bridge_windows(pci_function_t* restrict bridge, pci_window_list_t* restrict list)
{
    uint8_t io_base = pci_read_config8(bridge, PCI_CFG_IO_BASE);
    uint8_t io_limit = pci_read_config8(bridge, PCI_CFG_IO_LIMIT);
    uint64_t io_start = (uint64_t)(io_base & PCI_BRIDGE_IO_MASK) << 8;
    uint64_t io_end = ((uint64_t)(io_limit & PCI_BRIDGE_IO_MASK) << 8) + 0x1000;

    if(io_base & PCI_BRIDGE_IO_32BIT) {
        io_start |= (uint64_t)(pci_read_config16(bridge, PCI_CFG_IO_BASE_UPPER)) << 16;
        io_end += (uint64_t)(pci_read_config16(bridge, PCI_CFG_IO_LIMIT_UPPER)) << 16;
    }

    add_window(list, true, false, io_start, io_end);

    uint16_t memory_base = pci_read_config16(bridge, PCI_CFG_MEMORY_BASE);
    uint16_t memory_limit = pci_read_config16(bridge, PCI_CFG_MEMORY_LIMIT);
    uint64_t memory_start = (uint64_t)(memory_base & PCI_BRIDGE_WINDOW_MASK) << 16;
    uint64_t memory_end = ((uint64_t)(memory_limit & PCI_BRIDGE_WINDOW_MASK) << 16) + 0x100000;

    add_window(list, false, false, memory_start, memory_end);

    uint16_t pref_base = pci_read_config16(bridge, PCI_CFG_PREF_BASE);
    uint16_t pref_limit = pci_read_config16(bridge, PCI_CFG_PREF_LIMIT);
    uint64_t pref_start = (uint64_t)(pref_base & PCI_BRIDGE_WINDOW_MASK) << 16;
    uint64_t pref_end = ((uint64_t)(pref_limit & PCI_BRIDGE_WINDOW_MASK) << 16) + 0x100000;

    if(pref_base & PCI_BRIDGE_PREF_64BIT) {
        pref_start |= (uint64_t)(pci_read_config32(bridge, PCI_CFG_PREF_BASE_UPPER)) << 32;
        pref_end += (uint64_t)(pci_read_config32(bridge, PCI_CFG_PREF_LIMIT_UPPER)) << 32;
    }

    add_window(list, false, true, pref_start, pref_end);
}

// The host bridge says nothing of prefetching, so its memory windows take any BAR
static void read_root_windows(const pci_root_t* restrict root, pci_window_list_t* restrict list)
{
    for(size_t i = 0; i < root->num_windows; ++i) {
        const fwnode_range_t* range = &root->windows[i].range;
        bool is_io = root->windows[i].type == FWNODE_RESOURCE_PORT;

        add_window(list, is_io, false, range->start, range->start + range->length);
    }
}

static void read_bus_windows(const pci_bus_t* restrict bus, pci_window_list_t* restrict list)
{
    list->num_windows = 0;

    if(bus->bridge) {
        read_bridge_windows(bus->bridge, list);
    }
    else {
        read_root_windows(bus->root, list);
    }
}

static bool fits_window(const pci_window_t* restrict window, const pci_bar_t* restrict bar)
{
    bool is_io = bar->flags & PCI_BAR_IO;
    bool prefetchable = bar->flags & PCI_BAR_PREFETCH;

    return window->is_io == is_io && (prefetchable || !window->prefetchable);
}

static bool is_in_windows(const pci_window_list_t* restrict list, const pci_bar_t* restrict bar)
{
    for(size_t i = 0; i < list->num_windows; ++i) {
        const pci_window_t* window = &list->windows[i];

        if(fits_window(window, bar) && bar->address >= window->start && bar->address + bar->length <= window->end) {
            return true;
        }
    }

    return false;
}

static bool overlaps(uint64_t start, uint64_t end, uint64_t other_start, uint64_t other_end)
{
    return start < other_end && other_start < end;
}

// What a function takes of its bus's space: its own BARs except one, and the windows of a bridge
static bool function_takes(pci_function_t* restrict function, const pci_bar_t* except, bool is_io, uint64_t start, uint64_t end,
    uint64_t* restrict taken_end)
{
    for(unsigned i = 0; i < PCI_BAR_MAX; ++i) {
        const pci_bar_t* bar = &function->priv.bars[i];
        bool bar_is_io = bar->flags & PCI_BAR_IO;

        bool usable = bar != except && bar->length && bar->assigned;
        bool same_kind = bar_is_io == is_io;

        if(usable && same_kind && overlaps(start, end, bar->address, bar->address + bar->length)) {
            *taken_end = bar->address + bar->length;
            return true;
        }
    }

    if(!function->priv.is_bridge) {
        return false;
    }

    pci_window_list_t list;
    list.num_windows = 0;
    read_bridge_windows(function, &list);

    for(size_t i = 0; i < list.num_windows; ++i) {
        const pci_window_t* window = &list.windows[i];

        if(window->is_io == is_io && overlaps(start, end, window->start, window->end)) {
            *taken_end = window->end;
            return true;
        }
    }

    return false;
}

static bool bus_takes(const pci_bus_t* restrict bus, pci_function_t* restrict function, const pci_bar_t* except, bool is_io, uint64_t start,
    uint64_t end, uint64_t* restrict taken_end)
{
    if(function_takes(function, except, is_io, start, end, taken_end)) {
        return true;
    }

    for(pci_function_t* sibling = bus->functions; sibling; sibling = sibling->priv.next) {
        if(sibling != function && function_takes(sibling, nullptr, is_io, start, end, taken_end)) {
            return true;
        }
    }

    return false;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// First fit; a BAR's length is a power of two, which it is also aligned to
static bool find_room(const pci_bus_t* restrict bus, pci_function_t* restrict function, const pci_window_t* restrict window,
    const pci_bar_t* restrict bar, uint64_t* restrict address)
{
    bool is_io = window->is_io;
    uint64_t reach = ADDRESS_SPACE_END;

    if(is_io) {
        reach = IO_SPACE_END;
    }
    else if(0 == (bar->flags & PCI_BAR_MEM_64BIT)) {
        reach = BELOW_4G_END;
    }

    uint64_t end = window->end < reach ? window->end : reach;
    uint64_t start = window->start;

    if(is_io && start < IO_SPACE_START) {
        start = IO_SPACE_START;
    }

    start = align_up(start, bar->length);

    while(start < end && bar->length <= end - start) {
        uint64_t taken_end;

        if(!bus_takes(bus, function, bar, is_io, start, start + bar->length, &taken_end)) {
            *address = start;
            return true;
        }

        start = align_up(taken_end, bar->length);
    }

    return false;
}

// A prefetchable BAR tries the prefetchable windows first, to leave the others to those that can't go there
static bool place_bar(const pci_bus_t* restrict bus, pci_function_t* restrict function, const pci_window_list_t* restrict list,
    const pci_bar_t* restrict bar, uint64_t* restrict address)
{
    for(unsigned pass = 0; pass < 2; ++pass) {
        bool want_prefetchable = pass == 0;

        for(size_t i = 0; i < list->num_windows; ++i) {
            const pci_window_t* window = &list->windows[i];

            if(window->prefetchable == want_prefetchable && fits_window(window, bar) && find_room(bus, function, window, bar, address)) {
                return true;
            }
        }
    }

    return false;
}

static void write_bar(pci_function_t* restrict function, unsigned index, uint64_t address)
{
    uintptr_t offset = PCI_CFG_BAR0 + index * 4;
    uint32_t raw = pci_read_config32(function, offset);
    uint32_t low_bits = raw & ~(uint32_t)(PCI_BAR_ADDR_MASK_MEM);

    if(raw & PCI_BAR_IO) {
        low_bits = raw & ~(uint32_t)(PCI_BAR_ADDR_MASK_IO);
    }

    pci_write_config32(function, offset, (uint32_t)(address) | low_bits);

    if(function->priv.bars[index].flags & PCI_BAR_MEM_64BIT) {
        pci_write_config32(function, offset + 4, (uint32_t)(address >> 32));
    }
}

static uint32_t probe_bar_mask(pci_function_t* restrict function, uintptr_t offset)
{
    uint32_t raw = pci_read_config32(function, offset);
    pci_write_config32(function, offset, UINT32_C(0xFFFFFFFF));
    uint32_t mask = pci_read_config32(function, offset);
    pci_write_config32(function, offset, raw);
    return mask;
}

// PCI Local Bus 3.0, 6.2.5.1: the bits that stay zero
// after writing all ones give the BAR's length
static void read_bar(pci_function_t* restrict function, unsigned index)
{
    pci_bar_t* bar = &function->priv.bars[index];
    uintptr_t offset = PCI_CFG_BAR0 + index * 4;
    uint32_t raw = pci_read_config32(function, offset);
    bool is_64bit = 0 == (raw & PCI_BAR_IO) && (raw & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_64BIT && index + 1 < PCI_BAR_MAX;

    uint64_t mask = probe_bar_mask(function, offset);
    uint64_t address = raw;

    if(is_64bit) {
        mask |= (uint64_t)(probe_bar_mask(function, offset + 4)) << 32;
        address |= (uint64_t)(pci_read_config32(function, offset + 4)) << 32;
    }
    else {
        mask |= UINT64_C(0xFFFFFFFF00000000);
    }

    if(raw & PCI_BAR_IO) {
        // I/O BARs may hardwire the upper 16 bits to zero
        mask = (mask & PCI_BAR_ADDR_MASK_IO) | UINT64_C(0xFFFFFFFFFFFF0000);
        address &= PCI_BAR_ADDR_MASK_IO;
        bar->flags = PCI_BAR_IO;
    }
    else {
        mask &= PCI_BAR_ADDR_MASK_MEM;
        address &= PCI_BAR_ADDR_MASK_MEM;
        bar->flags = raw & (PCI_BAR_MEM_64BIT | PCI_BAR_PREFETCH);
    }

    bar->address = address;
    bar->length = 0;
    bar->assigned = false;

    if(mask != UINT64_C(0xFFFFFFFF00000000) && mask != UINT64_C(0xFFFFFFFFFFFF0000)) {
        bar->length = ~mask + 1;
        bar->assigned = address;
    }
}

// The firmware may leave two BARs at the same address, or a BAR in the window of a bridge on the same bus
static bool collides(const pci_bus_t* restrict bus, pci_function_t* restrict function, const pci_bar_t* restrict bar)
{
    uint64_t taken_end;

    return bus_takes(bus, function, bar, bar->flags & PCI_BAR_IO, bar->address, bar->address + bar->length, &taken_end);
}

// The space decodes only once all of its BARs have room
static space_decode_t assign_space(const pci_bus_t* restrict bus, pci_function_t* restrict function, const pci_window_list_t* restrict list,
    bool is_io)
{
    bool all_assigned = true;
    bool any = false;

    for(unsigned i = 0; i < PCI_BAR_MAX; ++i) {
        pci_bar_t* bar = &function->priv.bars[i];

        if(bar->length == 0 || (bool)(bar->flags & PCI_BAR_IO) != is_io) {
            continue;
        }

        any = true;

        if(bar->assigned && !is_in_windows(list, bar)) {
            pr_warning("pci: %s BAR%u at %p is outside the windows of its bus", function->priv.device->name, i,
                (void*)(uintptr_t)(bar->address));
        }
        else if(bar->assigned && collides(bus, function, bar)) {
            pr_warning("pci: %s BAR%u at %p overlaps another BAR or a bridge window", function->priv.device->name, i,
                (void*)(uintptr_t)(bar->address));
        }
        else if(bar->assigned) {
            continue;
        }

        uint64_t address;
        bar->assigned = false;

        if(!place_bar(bus, function, list, bar, &address)) {
            pr_warning("pci: %s BAR%u of 0x%llX bytes has no room", function->priv.device->name, i, (unsigned long long)(bar->length));
            all_assigned = false;
            continue;
        }

        write_bar(function, i, address);
        bar->address = address;
        bar->assigned = true;

        pr_verbose("pci: %s BAR%u at %p", function->priv.device->name, i, (void*)(uintptr_t)(address));
    }

    if(!any) {
        return SPACE_UNUSED;
    }

    if(!all_assigned) {
        for(unsigned i = 0; i < PCI_BAR_MAX; ++i) {
            pci_bar_t* bar = &function->priv.bars[i];

            if(bar->length && is_io == (bool)(bar->flags & PCI_BAR_IO)) {
                bar->assigned = false;
            }
        }

        return SPACE_OFF;
    }

    return SPACE_ON;
}

// A bridge forwards through its windows only while its spaces decode,
// so a BAR of its own that has no room doesn't turn them off
static uint16_t apply_decode(uint16_t command, uint16_t bit, space_decode_t decode, bool is_bridge)
{
    if(decode == SPACE_ON) {
        return command | bit;
    }

    if(decode == SPACE_OFF && !is_bridge) {
        return command & ~bit;
    }

    return command;
}

void pci_read_bars(pci_function_t* restrict function)
{
    uint16_t command = pci_read_config16(function, PCI_CFG_COMMAND);
    pci_write_config16(function, PCI_CFG_COMMAND, command & ~(uint16_t)(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE));

    unsigned num_bars = function->priv.is_bridge ? PCI_BRIDGE_BAR_MAX : PCI_BAR_MAX;

    for(unsigned i = 0; i < PCI_BAR_MAX; ++i) {
        pci_bar_t* bar = &function->priv.bars[i];
        *bar = (pci_bar_t) {};

        // The bus gives a compatibility channel its ISA addresses, and its BARs stay as they are
        if(i >= num_bars || pci_ide_is_compat(function, i / PCI_IDE_BARS_PER_CHANNEL)) {
            continue;
        }

        read_bar(function, i);

        if(bar->flags & PCI_BAR_MEM_64BIT) {
            i += 1;
            function->priv.bars[i] = (pci_bar_t) {};
        }
    }

    pci_write_config16(function, PCI_CFG_COMMAND, command);
}

void pci_assign_bars(pci_function_t* restrict function)
{
    const pci_bus_t* bus = function->priv.bus;

    pci_window_list_t list;
    read_bus_windows(bus, &list);

    space_decode_t io_decode = assign_space(bus, function, &list, true);
    space_decode_t memory_decode = assign_space(bus, function, &list, false);
    bool is_bridge = function->priv.is_bridge;

    uint16_t command = pci_read_config16(function, PCI_CFG_COMMAND);
    command = apply_decode(command, PCI_CMD_IO_SPACE, io_decode, is_bridge);
    command = apply_decode(command, PCI_CMD_MEM_SPACE, memory_decode, is_bridge);

    pci_write_config16(function, PCI_CFG_COMMAND, command);
}
