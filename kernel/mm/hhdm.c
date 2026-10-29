#include <limine.h>
#include <vex/hhdm.h>
#include <vex/initcall.h>
#include <vex/panic.h>

virt_addr_t hhdm_base;

static volatile __used __section(".limine_requests") struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

static void init_hhdm(void)
{
    if(hhdm_request.response == nullptr) {
        panic("bootloader did not provide HHDM information");
    }

    hhdm_base = virt_addr(hhdm_request.response->offset);
}

initcall_bsp_bootinfo(init_hhdm, INIT_FIRST);
