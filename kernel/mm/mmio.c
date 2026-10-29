#include <vex/assert.h>
#include <vex/errno.h>
#include <vex/kprintf.h>
#include <vex/mmio.h>
#include <vex/page.h>
#include <vex/pmap.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/tlb.h>

#define LARGE_ALIGN (UINT64_C(1) << 21)

typedef struct mmio_mapping_s mmio_mapping_t;

struct mmio_mapping_s {
    mmio_mapping_t* next; // Sorted by virt
    uintptr_t virt;
    phys_addr_t phys;
    size_t size;
    unsigned vprot;
    unsigned refcount; // Zero while the range waits for a shootdown, so it can't be reused yet
};

static spinlock_t mmio_lock = SPINLOCK_INIT;
static mmio_mapping_t* mmio_head = nullptr;

// A physical range may be mapped more than once
// only with one memory type; x86 leaves the result
// undefined when two mappings disagree
static bool type_conflicts_nolock(phys_addr_t phys, size_t size, unsigned vprot)
{
    for(mmio_mapping_t* it = mmio_head; it; it = it->next) {
        phys_addr_t it_end = address_add(it->phys, it->size);
        phys_addr_t end = address_add(phys, size);
        bool overlaps = address_less(phys, it_end) && address_less(it->phys, end);

        if(overlaps && (it->vprot & VPROT_CACHE_MASK) != (vprot & VPROT_CACHE_MASK)) {
            return true;
        }
    }

    return false;
}

static mmio_mapping_t* find_reusable_nolock(phys_addr_t phys, size_t size, unsigned vprot)
{
    for(mmio_mapping_t* it = mmio_head; it; it = it->next) {
        phys_addr_t it_end = address_add(it->phys, it->size);
        phys_addr_t end = address_add(phys, size);
        bool inside = !address_less(phys, it->phys) && !address_less(it_end, end);

        if(it->refcount && it->vprot == vprot && inside) {
            return it;
        }
    }

    return nullptr;
}

static uintptr_t place_virt(uintptr_t gap_start, phys_addr_t phys, size_t size)
{
    if(size < LARGE_ALIGN) {
        return gap_start;
    }

    uintptr_t virt = (gap_start & ~(LARGE_ALIGN - 1)) | (phys.value & (LARGE_ALIGN - 1));

    if(virt < gap_start) {
        virt += LARGE_ALIGN;
    }

    return virt;
}

static bool insert_nolock(mmio_mapping_t* restrict mapping)
{
    uintptr_t gap_start = MMIO_REGION_BASE;
    mmio_mapping_t** prev = &mmio_head;

    for(;;) {
        uintptr_t gap_end = MMIO_REGION_BASE + MMIO_REGION_SIZE;

        if(*prev) {
            gap_end = (*prev)->virt;
        }

        uintptr_t virt = place_virt(gap_start, mapping->phys, mapping->size);

        if(virt <= gap_end && gap_end - virt >= mapping->size) {
            mapping->virt = virt;
            mapping->next = *prev;
            *prev = mapping;
            return true;
        }

        if(*prev == nullptr) {
            return false;
        }

        gap_start = (*prev)->virt + (*prev)->size;
        prev = &(*prev)->next;
    }
}

static void unlink_nolock(mmio_mapping_t* restrict mapping)
{
    for(mmio_mapping_t** prev = &mmio_head; *prev; prev = &(*prev)->next) {
        if(*prev == mapping) {
            *prev = mapping->next;
            return;
        }
    }
}

void* mmio_map(phys_addr_t phys, size_t size, unsigned vprot)
{
    kassert(size > 0);
    kassert(0 == (vprot & VPROT_USER));

    phys_addr_t phys_page = page_align(phys);
    phys_addr_t end = address_add(phys, size);
    phys_addr_t phys_end = page_align_up(end);
    size_t length = address_diff(phys_end, phys_page);

    mmio_mapping_t* mapping = slab_alloc(sizeof(mmio_mapping_t));

    if(mapping == nullptr) {
        return nullptr;
    }

    mapping->phys = phys_page;
    mapping->size = length;
    mapping->vprot = vprot;
    mapping->refcount = 1;

    spinlock_acquire(&mmio_lock);

    mmio_mapping_t* existing = find_reusable_nolock(phys_page, length, vprot);

    if(existing) {
        existing->refcount += 1;
        spinlock_release(&mmio_lock);
        slab_free(mapping);
        return (void*)(existing->virt + address_diff(phys, existing->phys));
    }

    if(type_conflicts_nolock(phys_page, length, vprot)) {
        spinlock_release(&mmio_lock);
        slab_free(mapping);
        pr_warning("mmio: %#zx+%#zx is already mapped with another memory type", phys.value, size);
        return nullptr;
    }

    if(!insert_nolock(mapping)) {
        spinlock_release(&mmio_lock);
        slab_free(mapping);
        return nullptr;
    }

    int retval = pmap_map_range(&sys_pmap, virt_addr(mapping->virt), phys_page, length, vprot);

    if(retval < 0) {
        int unmapped = pmap_unmap_range_noflush(&sys_pmap, virt_addr(mapping->virt), length);
        kassert(unmapped == 0); // Nothing was split, so nothing had to be allocated

        unlink_nolock(mapping);
        spinlock_release(&mmio_lock);
        slab_free(mapping);
        return nullptr;
    }

    spinlock_release(&mmio_lock);

    return (void*)(mapping->virt + address_diff(phys, phys_page));
}

void mmio_unmap(volatile void* virt)
{
    uintptr_t address = (uintptr_t)(virt);

    spinlock_acquire(&mmio_lock);

    mmio_mapping_t* mapping = mmio_head;

    while(mapping && !(address >= mapping->virt && address < mapping->virt + mapping->size)) {
        mapping = mapping->next;
    }

    kassert(mapping);

    mapping->refcount -= 1;

    if(mapping->refcount > 0) {
        spinlock_release(&mmio_lock);
        return;
    }

    // Whole pages that were mapped as a whole, so nothing gets split
    int retval = pmap_unmap_range_noflush(&sys_pmap, virt_addr(mapping->virt), mapping->size);
    kassert(retval == 0);

    spinlock_release(&mmio_lock);

    tlb_shootdown();

    spinlock_acquire(&mmio_lock);
    unlink_nolock(mapping);
    spinlock_release(&mmio_lock);

    slab_free(mapping);
}

bool mmio_owns(const volatile void* virt)
{
    uintptr_t address = (uintptr_t)(virt);
    return address >= MMIO_REGION_BASE && address - MMIO_REGION_BASE < MMIO_REGION_SIZE;
}
