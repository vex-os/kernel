#include <limine.h>
#include <vex/assert.h>
#include <vex/cpucaps.h>
#include <vex/errno.h>
#include <vex/hhdm.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pmap.h>
#include <vex/pslab.h>
#include <vex/string.h>

static volatile __used __section(".limine_requests") struct limine_paging_mode_request paging_mode = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .mode = LIMINE_PAGING_MODE_X86_64_5LVL,
    .revision = 2,
};

unsigned pmap_detect_levels(void)
{
    if(paging_mode.response == nullptr) {
        panic("mmu: bootloader did not negotiate paging mode");
    }

    if(paging_mode.response->mode >= LIMINE_PAGING_MODE_X86_64_5LVL) {
        return 5;
    }

    if(paging_mode.response->mode >= LIMINE_PAGING_MODE_X86_64_4LVL) {
        return 4;
    }

    return 3;
}

unsigned pmap_detect_leaf_levels(void)
{
    if(cpu_has(CPU_FEATURE_PAGE_1G))
        return 3;
    return 2;
}

int arch_pmap_alloc(pmap_t* restrict pmap)
{
    pmap->phys = pslab_alloc_phys();

    if(address_null(pmap->phys)) {
        return -ENOMEM;
    }

    pmap->virt = phys_to_virt(pmap->phys);

    arch_pmap_zero(pmap);

    return 0;
}

void arch_pmap_free(pmap_t* restrict pmap)
{
    pslab_free_virt(pmap->virt);

    pmap->phys = phys_addr(0);
    pmap->virt = nullptr;
}

void arch_pmap_zero(pmap_t* restrict pmap)
{
    memset(pmap->virt, 0, PAGE_SIZE);
}

void arch_pmap_kcopy(pmap_t* restrict dest, const pmap_t* restrict source)
{
    for(size_t i = AMD64_PAGEMAP_START_KERN; i < PAGEMAP_SIZE; ++i) {
        dest->virt[i] = source->virt[i];
    }
}
