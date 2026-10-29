#include <vex/initcall.h>
#include <vex/memmap.h>
#include <vex/panic.h>

const struct limine_memmap_response* memmap;

static volatile __used __section(".limine_requests") struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

bool memmap_is_ram_type(uint64_t type)
{
    switch(type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return true;
        default:
            return false;
    }
}

bool memmap_is_ram(phys_addr_t phys, size_t size)
{
    // The entries of the map are numbers from the bootloader
    uintptr_t address = phys.value;
    uintptr_t end = address + size;

    while(address < end) {
        const struct limine_memmap_entry* found = nullptr;

        for(size_t i = 0; i < memmap->entry_count; ++i) {
            const struct limine_memmap_entry* entry = memmap->entries[i];

            if(address >= entry->base && address - entry->base < entry->length) {
                found = entry;
                break;
            }
        }

        if(found == nullptr || !memmap_is_ram_type(found->type)) {
            return false;
        }

        address = found->base + found->length;
    }

    return true;
}

static void init_memmap(void)
{
    if(memmap_request.response == nullptr || memmap_request.response->entry_count == 0) {
        panic("no valid memory map provided by bootloader");
    }

    memmap = memmap_request.response;
}

initcall_bsp_bootinfo(init_memmap, INIT_FIRST);
