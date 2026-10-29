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

static __always_inline uintptr_t slot_base(size_t slot)
{
    return KSTACK_REGION_BASE + slot * KSTACK_SLOT_SIZE;
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

static void unmap_pages(uintptr_t stack_bottom, size_t count)
{
    phys_addr_t phys[KSTACK_PAGES];

    for(size_t i = 0U; i < count; ++i) {
        uintptr_t virt = stack_bottom + i * PAGE_SIZE;
        phys[i] = page_align(pmap_resolve_address(&sys_pmap, virt_addr(virt)));
        pmap_unmap_noflush(&sys_pmap, virt_addr(virt));
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

    uintptr_t stack_bottom = slot_base(slot) + PAGE_SIZE;

    for(size_t i = 0U; i < KSTACK_PAGES; ++i) {
        phys_addr_t phys = pslab_alloc_phys();

        if(address_null(phys) || pmap_map(&sys_pmap, virt_addr(stack_bottom + i * PAGE_SIZE), phys, VPROT_RW) < 0) {
            pslab_free_phys(phys);
            unmap_pages(stack_bottom, i);
            release_slot(slot);
            return nullptr;
        }
    }

    return (void*)(stack_bottom + KSTACK_SIZE);
}

void kstack_free(void* restrict top)
{
    if(top == nullptr) {
        return;
    }

    uintptr_t stack_top = (uintptr_t)(top);
    uintptr_t stack_bottom = stack_top - KSTACK_SIZE;
    size_t slot = (stack_bottom - PAGE_SIZE - KSTACK_REGION_BASE) / KSTACK_SLOT_SIZE;

    if(stack_top < KSTACK_REGION_BASE || slot >= KSTACK_NUM_SLOTS || slot_base(slot) + KSTACK_SLOT_SIZE != stack_top) {
        panic("kstack: freeing invalid kernel stack %p", top);
    }

    unmap_pages(stack_bottom, KSTACK_PAGES);
    release_slot(slot);
}

bool kstack_is_guard(uintptr_t address)
{
    if(address < KSTACK_REGION_BASE) {
        return false;
    }

    size_t offset = address - KSTACK_REGION_BASE;

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
