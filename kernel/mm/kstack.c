#include <vex/bitmap.h>
#include <vex/initcall.h>
#include <vex/kstack.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pmap.h>
#include <vex/pslab.h>
#include <vex/spinlock.h>
#include <vex/tlb.h>

// Each slot is a guard page followed by the stack itself; the guard page
// is never mapped, so running off the bottom of a stack faults right away
#define KSTACK_SLOT_SIZE ((KSTACK_PAGES + 1U) * (size_t)(PAGE_SIZE))
#define KSTACK_NUM_SLOTS ((unsigned)(CONFIG_MAX_THREADS))

static_assert(KSTACK_NUM_SLOTS % BITMAP_CHUNK_BITS == 0);
static_assert(KSTACK_NUM_SLOTS * KSTACK_SLOT_SIZE <= KSTACK_REGION_SIZE);

static bitmap_t free_slots[KSTACK_NUM_SLOTS / BITMAP_CHUNK_BITS];
static size_t next_slot = 0U;
static spinlock_t slots_lock = SPINLOCK_INIT;

static __always_inline virt_addr_t slot_base(size_t slot)
{
    return virt_addr(KSTACK_REGION_BASE + slot * KSTACK_SLOT_SIZE);
}

static size_t claim_slot(void)
{
    bool intr_state = spinlock_acquire_intr(&slots_lock);

    for(size_t i = 0U; i < KSTACK_NUM_SLOTS; ++i) {
        size_t slot = (next_slot + i) % KSTACK_NUM_SLOTS;

        if(bitmap_isset(free_slots, slot)) {
            bitmap_clear(free_slots, slot);
            next_slot = slot + 1U;
            spinlock_release_intr(&slots_lock, intr_state);
            return slot;
        }
    }

    spinlock_release_intr(&slots_lock, intr_state);

    return SIZE_MAX;
}

static void release_slot(size_t slot)
{
    bool intr_state = spinlock_acquire_intr(&slots_lock);
    bitmap_set(free_slots, slot);
    spinlock_release_intr(&slots_lock, intr_state);
}

static void unmap_pages(virt_addr_t stack_bottom, size_t count)
{
    phys_addr_t phys[KSTACK_PAGES];

    for(size_t i = 0U; i < count; ++i) {
        virt_addr_t virt = address_add(stack_bottom, i * PAGE_SIZE);
        phys[i] = page_align(pmap_resolve_address(&sys_pmap, virt));
        pmap_unmap_noflush(&sys_pmap, virt);
    }

    tlb_shootdown();

    for(size_t i = 0U; i < count; ++i) {
        pslab_free_phys(phys[i]);
    }
}

void* kstack_alloc(void)
{
    size_t slot = claim_slot();

    if(slot == SIZE_MAX) {
        return nullptr;
    }

    virt_addr_t slot_start = slot_base(slot);
    virt_addr_t stack_bottom = address_add(slot_start, PAGE_SIZE);

    for(size_t i = 0U; i < KSTACK_PAGES; ++i) {
        phys_addr_t phys = pslab_alloc_phys();
        virt_addr_t virt = address_add(stack_bottom, i * PAGE_SIZE);

        if(address_null(phys) || pmap_map(&sys_pmap, virt, phys, VPROT_RW) < 0) {
            pslab_free_phys(phys);
            unmap_pages(stack_bottom, i);
            release_slot(slot);
            return nullptr;
        }
    }

    virt_addr_t top = address_add(stack_bottom, KSTACK_SIZE);

    return virt_addr_pointer(top);
}

void kstack_free(void* restrict top)
{
    if(top == nullptr) {
        return;
    }

    virt_addr_t stack_top = virt_addr_from_pointer(top);
    virt_addr_t region = virt_addr(KSTACK_REGION_BASE);
    bool in_region = address_greater_equal(stack_top, region);

    // An address below the region has no slot
    size_t slot = SIZE_MAX;

    if(in_region) {
        slot = (address_diff(stack_top, region) - KSTACK_SIZE - PAGE_SIZE) / KSTACK_SLOT_SIZE;
    }

    if(slot >= KSTACK_NUM_SLOTS) {
        panic("kstack: freeing invalid kernel stack %p", top);
    }

    virt_addr_t slot_start = slot_base(slot);
    virt_addr_t slot_end = address_add(slot_start, KSTACK_SLOT_SIZE);

    if(!address_equal(slot_end, stack_top)) {
        panic("kstack: freeing invalid kernel stack %p", top);
    }

    virt_addr_t stack_bottom = address_sub(stack_top, KSTACK_SIZE);

    unmap_pages(stack_bottom, KSTACK_PAGES);
    release_slot(slot);
}

bool kstack_is_guard(virt_addr_t address)
{
    virt_addr_t region = virt_addr(KSTACK_REGION_BASE);

    if(address_less(address, region)) {
        return false;
    }

    size_t offset = address_diff(address, region);

    if(offset >= KSTACK_NUM_SLOTS * KSTACK_SLOT_SIZE) {
        return false;
    }

    return (offset % KSTACK_SLOT_SIZE) < PAGE_SIZE;
}

static void init_kstack(void)
{
    bitmap_range_set(free_slots, 0U, KSTACK_NUM_SLOTS - 1U);
}

initcall_bsp_heap(init_kstack, INIT_MIDDLE);
