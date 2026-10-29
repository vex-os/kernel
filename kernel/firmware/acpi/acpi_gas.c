#include <uacpi/acpi.h>
#include <vex/acpi.h>
#include <vex/errno.h>
#include <vex/mmio.h>
#include <vex/vprot.h>

int acpi_gas_region(const struct acpi_gas* restrict gas, size_t length, io_region_t* restrict region)
{
    if(gas->address_space_id == ACPI_AS_ID_SYS_IO) {
        region->space = IO_SPACE_PORT;
        region->base = gas->address;
        region->length = length;
        return 0;
    }

    if(gas->address_space_id != ACPI_AS_ID_SYS_MEM) {
        return -EINVAL;
    }

    void* virt = mmio_map(phys_addr(gas->address), length, VPROT_MMIO);

    if(virt == nullptr) {
        return -ENOMEM;
    }

    region->space = IO_SPACE_MEMORY;
    region->base = (uintptr_t)(virt);
    region->length = length;

    return 0;
}
