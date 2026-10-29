#include <limine.h>
#include <vex/initcall.h>
#include <vex/kbase.h>
#include <vex/panic.h>

phys_addr_t kernel_base_phys;
virt_addr_t kernel_base_virt;

static volatile __used __section(".limine_requests") struct limine_executable_address_request kbase_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

static void init_kbase(void)
{
    if(kbase_request.response == nullptr) {
        panic("bootloader did not provide executable address information");
    }

    kernel_base_phys = phys_addr(kbase_request.response->physical_base);
    kernel_base_virt = virt_addr(kbase_request.response->virtual_base);
}

initcall_bsp_bootinfo(init_kbase, INIT_FIRST);
