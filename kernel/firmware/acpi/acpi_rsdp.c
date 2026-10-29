#include <limine.h>
#include <vex/hhdm.h>
#include <vex/initcall.h>
#include <vex/panic.h>
#include <vex/rsdp.h>

static volatile __used __section(".limine_requests") struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

phys_addr_t rsdp_address = {};

static void init_acpi_rsdp(void)
{
    if(rsdp_request.response == nullptr || rsdp_request.response->address == 0) {
        panic("bootloader did not provide RSDP information");
    }

    rsdp_address = virt_to_phys(rsdp_request.response->address);
}

initcall_bsp_bootinfo(init_acpi_rsdp, INIT_LAST);
