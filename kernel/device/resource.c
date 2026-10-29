#include <vex/errno.h>
#include <vex/intctl.h>
#include <vex/kmalloc.h>
#include <vex/mmio.h>
#include <vex/vprot.h>

#include "device_internal.h"

struct device_irq_s {
    device_irq_t* next;
    unsigned index;
    interrupt_action_t* action;
    bool by_bus; // The bus's attach_irq routed it. enable_irq and disable_irq don't apply
};

struct device_mapping_s {
    device_mapping_t* next;
    io_region_t region;
};

static int fw_get_resource(device_t* restrict device, bool want_irq, unsigned index, fwnode_resource_t* restrict resource)
{
    unsigned found = 0;

    for(size_t i = 0; i < device->priv.num_resources; ++i) {
        const fwnode_resource_t* candidate = &device->priv.resources[i];
        bool is_irq = candidate->type == FWNODE_RESOURCE_IRQ;
        bool wanted = want_irq ? is_irq : device_is_range(candidate);

        if(!wanted) {
            continue;
        }

        if(found == index) {
            *resource = *candidate;
            return 0;
        }

        found += 1;
    }

    return -ENODEV;
}

static int get_range(device_t* restrict device, unsigned index, fwnode_resource_t* restrict range)
{
    device_t* bus = device_bus_of(device);

    if(bus && bus->bus_ops->get_range) {
        return bus->bus_ops->get_range(bus, device, index, range);
    }

    return fw_get_resource(device, false, index, range);
}

static int get_irq(device_t* restrict device, unsigned index, fwnode_irq_t* restrict irq)
{
    device_t* bus = device_bus_of(device);

    if(bus && bus->bus_ops->get_irq) {
        return bus->bus_ops->get_irq(bus, device, index, irq);
    }

    fwnode_resource_t resource;
    int retval = fw_get_resource(device, true, index, &resource);

    if(retval < 0) {
        return retval;
    }

    *irq = resource.irq;

    return 0;
}

static device_irq_t** find_irq_nolock(device_t* restrict device, unsigned index)
{
    device_irq_t** prev = &device->priv.irqs;

    while(*prev && (*prev)->index != index) {
        prev = &(*prev)->next;
    }

    return prev;
}

static uint32_t gsi_of(const fwnode_irq_t* restrict irq, unsigned* restrict trigger, unsigned* restrict polarity)
{
    uint32_t gsi = irq->number;
    *trigger = irq->trigger;
    *polarity = irq->polarity;

    if(irq->flags & FWNODE_IRQ_ISA) {
        interrupt_isa_route(irq->number, &gsi, trigger, polarity);
    }

    return gsi;
}

static int use_controller(device_t* restrict device, uint32_t gsi)
{
    interrupt_controller_t* controller = interrupt_controller_find(gsi);

    if(controller == nullptr) {
        return -ENODEV;
    }

    if(controller->device == nullptr || controller->device == device) {
        return 0;
    }

    return device_use(device, controller->device);
}

static int attach_gsi(const fwnode_irq_t* restrict irq, irq_handler_t handler, void* restrict ctxdata, interrupt_action_t** restrict action)
{
    unsigned trigger;
    unsigned polarity;
    uint32_t gsi = gsi_of(irq, &trigger, &polarity);
    unsigned flags = 0;

    if(irq->flags & FWNODE_IRQ_SHARED) {
        flags |= INTERRUPT_SHARED;
    }

    return interrupt_attach(gsi, trigger, polarity, flags, handler, ctxdata, action);
}

static int attach_by_bus(device_t* restrict device, unsigned index, irq_handler_t handler, void* restrict ctxdata,
    interrupt_action_t** restrict action)
{
    device_t* bus = device_bus_of(device);

    if(bus == nullptr || bus->bus_ops->attach_irq == nullptr) {
        return -ENOENT;
    }

    return bus->bus_ops->attach_irq(bus, device, index, handler, ctxdata, action);
}

static int attach_by_controller(device_t* restrict device, unsigned index, irq_handler_t handler, void* restrict ctxdata,
    interrupt_action_t** restrict action)
{
    fwnode_irq_t irq;
    int retval = get_irq(device, index, &irq);

    if(retval < 0) {
        return retval;
    }

    unsigned trigger;
    unsigned polarity;
    retval = use_controller(device, gsi_of(&irq, &trigger, &polarity));

    if(retval < 0) {
        return retval;
    }

    return attach_gsi(&irq, handler, ctxdata, action);
}

static void enable_irq(device_t* restrict device, unsigned index)
{
    device_t* bus = device_bus_of(device);

    if(bus && bus->bus_ops->enable_irq) {
        bus->bus_ops->enable_irq(bus, device, index);
    }
}

static void disable_irq(device_t* restrict device, unsigned index)
{
    device_t* bus = device_bus_of(device);

    if(bus && bus->bus_ops->disable_irq) {
        bus->bus_ops->disable_irq(bus, device, index);
    }
}

static int map_range(device_t* restrict bus, unsigned type, uint64_t start, uint64_t length, unsigned vprot, io_region_t* restrict region)
{
    uint64_t address = start;
    int retval = device_translate_range(bus, &type, &address, length);

    if(retval < 0) {
        return retval;
    }

    if(type == FWNODE_RESOURCE_PORT) {
        region->space = IO_SPACE_PORT;
        region->base = address;
        region->length = length;
        return 0;
    }

    void* virt = mmio_map(phys_addr(address), length, vprot);

    if(virt == nullptr) {
        return -ENOMEM;
    }

    region->space = IO_SPACE_MEMORY;
    region->base = (uintptr_t)(virt);
    region->length = length;

    return 0;
}

void device_release_resources(device_t* restrict device)
{
    device_t* bus = device_bus_of(device);

    if(bus && bus->bus_ops->release_child) {
        bus->bus_ops->release_child(bus, device);
    }

    mutex_acquire(&device_tree_mutex);
    device_irq_t* irqs = device->priv.irqs;
    device_mapping_t* mappings = device->priv.mappings;
    device->priv.irqs = nullptr;
    device->priv.mappings = nullptr;
    mutex_release(&device_tree_mutex);

    while(irqs) {
        device_irq_t* next = irqs->next;

        if(!irqs->by_bus) {
            disable_irq(device, irqs->index);
        }

        interrupt_detach(irqs->action);
        kfree(irqs);
        irqs = next;
    }

    while(mappings) {
        device_mapping_t* next = mappings->next;
        device_unmap_range(&mappings->region);
        kfree(mappings);
        mappings = next;
    }
}

int device_translate_range(device_t* restrict bus, unsigned* restrict type, uint64_t* restrict address, uint64_t length)
{
    for(device_t* node = bus; node; node = node->parent) {
        if(node->bus_ops == nullptr || node->bus_ops->translate_range == nullptr) {
            continue;
        }

        int retval = node->bus_ops->translate_range(node, type, address, length);

        if(retval < 0) {
            return retval;
        }
    }

    return 0;
}

int device_map_range(device_t* restrict bus, unsigned type, uint64_t start, uint64_t length, io_region_t* restrict region)
{
    return map_range(bus, type, start, length, VPROT_MMIO, region);
}

void device_unmap_range(io_region_t* restrict region)
{
    if(region->space == IO_SPACE_MEMORY) {
        mmio_unmap((volatile void*)(region->base));
    }
}

int device_map_io(device_t* restrict device, unsigned index, io_region_t* restrict region)
{
    return device_map_io_vprot(device, index, VPROT_MMIO, region);
}

int device_map_io_vprot(device_t* restrict device, unsigned index, unsigned vprot, io_region_t* restrict region)
{
    fwnode_resource_t range;
    int retval = get_range(device, index, &range);

    if(retval < 0) {
        return retval;
    }

    device_mapping_t* mapping = kmalloc(sizeof(device_mapping_t));

    if(mapping == nullptr) {
        return -ENOMEM;
    }

    retval = map_range(device->parent, range.type, range.range.start, range.range.length, vprot, &mapping->region);

    if(retval < 0) {
        kfree(mapping);
        return retval;
    }

    *region = mapping->region;

    mutex_acquire(&device_tree_mutex);
    mapping->next = device->priv.mappings;
    device->priv.mappings = mapping;
    mutex_release(&device_tree_mutex);

    return 0;
}

int device_adopt_io(device_t* restrict device, const io_region_t* restrict region)
{
    device_mapping_t* mapping = kmalloc(sizeof(device_mapping_t));

    if(mapping == nullptr) {
        return -ENOMEM;
    }

    mapping->region = *region;

    mutex_acquire(&device_tree_mutex);
    mapping->next = device->priv.mappings;
    device->priv.mappings = mapping;
    mutex_release(&device_tree_mutex);

    return 0;
}

int device_get_range(device_t* restrict device, unsigned index, fwnode_resource_t* restrict range)
{
    return get_range(device, index, range);
}

int device_get_irq(device_t* restrict device, unsigned index, fwnode_irq_t* restrict irq)
{
    return get_irq(device, index, irq);
}

int device_get_io_address(device_t* restrict device, unsigned index, uint64_t* restrict address)
{
    fwnode_resource_t range;
    int retval = get_range(device, index, &range);

    if(retval < 0) {
        return retval;
    }

    unsigned type = range.type;
    uint64_t cpu_address = range.range.start;

    retval = device_translate_range(device->parent, &type, &cpu_address, range.range.length);

    if(retval < 0) {
        return retval;
    }

    if(type != FWNODE_RESOURCE_MEMORY) {
        return -EINVAL;
    }

    *address = cpu_address;

    return 0;
}

void device_unmap_io(device_t* restrict device, io_region_t* restrict region)
{
    mutex_acquire(&device_tree_mutex);

    device_mapping_t** prev = &device->priv.mappings;

    while(*prev && ((*prev)->region.space != region->space || (*prev)->region.base != region->base)) {
        prev = &(*prev)->next;
    }

    device_mapping_t* mapping = *prev;

    if(mapping) {
        *prev = mapping->next;
    }

    mutex_release(&device_tree_mutex);

    if(mapping) {
        device_unmap_range(&mapping->region);
        kfree(mapping);
    }
}

int device_request_irq(device_t* restrict device, unsigned index, irq_handler_t handler, void* restrict ctxdata)
{
    device_irq_t* entry = kmalloc(sizeof(device_irq_t));

    if(entry == nullptr) {
        return -ENOMEM;
    }

    mutex_acquire(&device_tree_mutex);
    bool taken = *find_irq_nolock(device, index);
    mutex_release(&device_tree_mutex);

    if(taken) {
        kfree(entry);
        return -EBUSY;
    }

    int retval = attach_by_bus(device, index, handler, ctxdata, &entry->action);
    entry->by_bus = retval != -ENOENT;

    if(retval == -ENOENT) {
        retval = attach_by_controller(device, index, handler, ctxdata, &entry->action);
    }

    if(retval < 0) {
        kfree(entry);
        return retval;
    }

    entry->index = index;

    mutex_acquire(&device_tree_mutex);

    entry->next = device->priv.irqs;
    device->priv.irqs = entry;

    if(!entry->by_bus) {
        enable_irq(device, index);
    }

    mutex_release(&device_tree_mutex);

    return 0;
}

void device_release_irq(device_t* restrict device, unsigned index)
{
    mutex_acquire(&device_tree_mutex);

    device_irq_t** prev = find_irq_nolock(device, index);
    device_irq_t* entry = *prev;

    if(entry) {
        *prev = entry->next;
    }

    mutex_release(&device_tree_mutex);

    if(entry) {
        if(!entry->by_bus) {
            disable_irq(device, index);
        }

        interrupt_detach(entry->action);
        kfree(entry);
    }
}
