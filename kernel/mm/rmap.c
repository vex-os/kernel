#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/page.h>
#include <vex/pframe.h>
#include <vex/pmap.h>
#include <vex/rmap.h>
#include <vex/slab.h>
#include <vex/spinlock.h>

#define NUM_LOCKS 256

static_assert(sizeof(rmap_entry_t) <= SLAB_MAX_SIZE);

// Taken with interrupts off, as the scheduler's interrupt releases address spaces
static spinlock_t locks[NUM_LOCKS];

static spinlock_t* frame_lock(phys_addr_t phys)
{
    return &locks[(phys.value / PAGE_SIZE) % NUM_LOCKS];
}

// Pins come and go without the vmspace lock, so only the mappings and the
// file cache decide whether a write may land in the frame instead of a copy
static bool sole_mapping_locked(const pframe_t* restrict pframe)
{
    const rmap_entry_t* first = pframe->mappings;

    if(first == nullptr || first->next) {
        return false;
    }

    return 0 == (atomic_load(&pframe->flags, ATOMIC_ACQUIRE) & PFRAME_CACHE);
}

static void count_mapped(pmap_t* restrict pmap)
{
    size_t resident = 1 + atomic_fetch_add(&pmap->resident_pages, 1, ATOMIC_RELAXED);
    size_t peak = atomic_load(&pmap->peak_pages, ATOMIC_RELAXED);

    while(resident > peak) {
        if(atomic_cmpxchg_weak(&pmap->peak_pages, &peak, resident, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
            break;
        }
    }
}

static void count_unmapped(pmap_t* restrict pmap)
{
    atomic_fetch_sub(&pmap->resident_pages, 1, ATOMIC_RELAXED);
}

static rmap_entry_t* unlink_entry(pframe_t* restrict pframe, pmap_t* restrict pmap, virt_addr_t virt)
{
    for(rmap_entry_t** prev = &pframe->mappings; *prev; prev = &(*prev)->next) {
        rmap_entry_t* entry = *prev;

        if(entry->pmap == pmap && address_equal(entry->virt, virt)) {
            *prev = entry->next;
            return entry;
        }
    }

    return nullptr;
}

int rmap_map(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t phys, unsigned vprot)
{
    rmap_entry_t* entry = slab_alloc(sizeof(rmap_entry_t));

    if(entry == nullptr) {
        return -ENOMEM;
    }

    entry->pmap = pmap;
    entry->virt = virt;

    pframe_t* pframe = pframe_of(phys);
    spinlock_t* lock = frame_lock(phys);

    bool intr_state = spinlock_acquire_intr(lock);

    int retval = pmap_map(pmap, virt, phys, vprot);

    if(retval == 0) {
        entry->next = pframe->mappings;
        pframe->mappings = entry;
        count_mapped(pmap);
    }

    spinlock_release_intr(lock, intr_state);

    if(retval < 0) {
        slab_free(entry);
    }

    return retval;
}

phys_addr_t rmap_unmap(pmap_t* restrict pmap, virt_addr_t virt)
{
    phys_addr_t phys = page_align(pmap_resolve_address(pmap, virt));

    if(address_null(phys)) {
        return phys_addr(0);
    }

    pframe_t* pframe = pframe_of(phys);
    spinlock_t* lock = frame_lock(phys);

    bool intr_state = spinlock_acquire_intr(lock);

    rmap_entry_t* entry = unlink_entry(pframe, pmap, virt);

    if(entry) {
        int retval = pmap_unmap_noflush(pmap, virt);
        kassert(retval == 0);
        count_unmapped(pmap);
    }

    spinlock_release_intr(lock, intr_state);

    if(entry == nullptr) {
        return phys_addr(0);
    }

    slab_free(entry);

    return phys;
}

int rmap_replace(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t old_phys, phys_addr_t new_phys, unsigned vprot)
{
    pframe_t* old_pf = pframe_of(old_phys);
    pframe_t* new_pf = pframe_of(new_phys);
    spinlock_t* lock = frame_lock(old_phys);

    bool intr_state = spinlock_acquire_intr(lock);

    rmap_entry_t* entry = unlink_entry(old_pf, pmap, virt);

    if(entry) {
        int retval = pmap_unmap_noflush(pmap, virt);
        kassert(retval == 0);

        retval = pmap_map(pmap, virt, new_phys, vprot);
        kassert(retval == 0); // The page table that held the old entry is still there

        entry->next = new_pf->mappings;
        new_pf->mappings = entry;
    }

    spinlock_release_intr(lock, intr_state);

    if(entry == nullptr) {
        return -EAGAIN;
    }

    return 0;
}

int rmap_patch(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t phys, unsigned vprot)
{
    spinlock_t* lock = frame_lock(phys);
    bool intr_state = spinlock_acquire_intr(lock);

    int retval = -EAGAIN;

    phys_addr_t current = page_align(pmap_resolve_address(pmap, virt));

    if(address_equal(phys, current)) {
        retval = pmap_patch_noflush(pmap, virt, vprot);
    }

    spinlock_release_intr(lock, intr_state);

    return retval;
}

phys_addr_t rmap_pin(pmap_t* restrict pmap, virt_addr_t virt, bool* restrict sole)
{
    phys_addr_t phys = page_align(pmap_resolve_address(pmap, virt));

    if(address_null(phys)) {
        return phys_addr(0);
    }

    pframe_t* pframe = pframe_of(phys);
    spinlock_t* lock = frame_lock(phys);

    bool intr_state = spinlock_acquire_intr(lock);

    phys_addr_t current = page_align(pmap_resolve_address(pmap, virt));

    bool mapped = address_equal(phys, current);

    if(mapped) {
        if(sole) {
            *sole = sole_mapping_locked(pframe);
        }

        pframe_incref(pframe);
    }

    spinlock_release_intr(lock, intr_state);

    if(!mapped) {
        return phys_addr(0);
    }

    return phys;
}

size_t rmap_unmap_all(pframe_t* restrict pframe)
{
    spinlock_t* lock = frame_lock(pframe_phys(pframe));

    bool intr_state = spinlock_acquire_intr(lock);

    rmap_entry_t* entries = pframe->mappings;
    pframe->mappings = nullptr;

    size_t count = 0;

    for(rmap_entry_t* it = entries; it; it = it->next) {
        int retval = pmap_unmap_noflush(it->pmap, it->virt);
        kassert(retval == 0);
        count_unmapped(it->pmap);
        count += 1;
    }

    spinlock_release_intr(lock, intr_state);

    while(entries) {
        rmap_entry_t* next = entries->next;
        slab_free(entries);
        entries = next;
    }

    return count;
}

size_t rmap_protect_all(pframe_t* restrict pframe)
{
    spinlock_t* lock = frame_lock(pframe_phys(pframe));
    bool intr_state = spinlock_acquire_intr(lock);

    size_t count = 0;

    for(rmap_entry_t* it = pframe->mappings; it; it = it->next) {
        unsigned vprot = pmap_resolve_vprot(it->pmap, it->virt);

        if(vprot & VPROT_WRITE) {
            int retval = pmap_patch_noflush(it->pmap, it->virt, vprot & ~VPROT_WRITE);
            kassert(retval == 0);
            count += 1;
        }
    }

    spinlock_release_intr(lock, intr_state);

    return count;
}

size_t rmap_peak_pages(const pmap_t* restrict pmap)
{
    return atomic_load(&pmap->peak_pages, ATOMIC_RELAXED);
}
