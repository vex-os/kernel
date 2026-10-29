#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/fcache.h>
#include <vex/hhdm.h>
#include <vex/hrsize.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/memmap.h>
#include <vex/page.h>
#include <vex/pdma.h>
#include <vex/pframe.h>
#include <vex/pslab.h>
#include <vex/spinlock.h>

#define RECLAIM_BATCH 32 // Pages freed from the file cache at a time
#define RESERVE_PAGES 64 // Kept free for callers that hold a spinlock and can't reclaim

static void** pslab_list = nullptr;
static spinlock_t pslab_lock = SPINLOCK_INIT;

size_t pslab_numpages = 0U;
size_t pslab_numfree = 0U;

static void pslab_add_region(phys_addr_t start, size_t length)
{
    void** head_ptr;
    size_t offset;

    for(offset = 0U; offset < length; offset += PAGE_SIZE) {
        phys_addr_t page = address_add(start, offset);
        pframe_t* pframe = pframe_of(page);

        pframe->flags |= PFRAME_PSLAB;

        head_ptr = phys_to_virt(page);
        head_ptr[0] = pslab_list;
        pslab_list = head_ptr;
        pslab_numpages += 1U;
        pslab_numfree += 1U;
    }

    pr_verbose("pslab: adding region: start=%p end=%p", (void*)(start.value), (void*)(address_add(start, length - 1U).value));
}

static void pslab_add_usable(phys_addr_t start, size_t length)
{
    phys_addr_t reserved_start;
    size_t reserved_length;
    pframe_storage(&reserved_start, &reserved_length);

    phys_addr_t end = address_add(start, length);
    phys_addr_t reserved_end = address_add(reserved_start, reserved_length);

    if(!address_less(start, reserved_end) || !address_less(reserved_start, end)) {
        pslab_add_region(start, length);
        return;
    }

    if(address_less(start, reserved_start)) {
        pslab_add_region(start, address_diff(reserved_start, start));
    }

    if(address_less(reserved_end, end)) {
        pslab_add_region(reserved_end, address_diff(end, reserved_end));
    }
}

static void* take_page(void)
{
    void** head_ptr;

    bool intr_state = spinlock_acquire_intr(&pslab_lock);

    if(pslab_list) {
        head_ptr = pslab_list;
        pslab_list = pslab_list[0];
        pslab_numfree -= 1U;

        pframe_t* pframe = pframe_of(virt_to_phys(head_ptr));
        kassert(pframe->refcount == 0);
        pframe->refcount = 1;

        spinlock_release_intr(&pslab_lock, intr_state);

        return head_ptr;
    }

    spinlock_release_intr(&pslab_lock, intr_state);

    return nullptr;
}

phys_addr_t pslab_alloc_phys(void)
{
    void* ptr = pslab_alloc_virt();
    if(ptr == nullptr)
        return phys_addr(0U);
    return virt_to_phys(ptr);
}

void* pslab_alloc_virt(void)
{
    void* ptr = take_page();

    if(ptr == nullptr) {
        if(fcache_reclaim(RECLAIM_BATCH)) {
            ptr = take_page();
        }

        return ptr;
    }

    if(atomic_load(&pslab_numfree, ATOMIC_RELAXED) < RESERVE_PAGES) {
        fcache_reclaim(RECLAIM_BATCH);
    }

    return ptr;
}

void pslab_free_phys(phys_addr_t phys)
{
    if(address_valid(phys)) {
        pslab_free_virt(phys_to_virt(phys));
    }
}

void pslab_free_virt(void* restrict virt)
{
    void** head_ptr;

    bool intr_state = spinlock_acquire_intr(&pslab_lock);

    if(virt) {
        pframe_t* pframe = pframe_of(virt_to_phys(virt));
        kassert(pframe->flags & PFRAME_PSLAB);
        kassert(pframe->refcount == 1);
        kassert(pframe->mappings == nullptr);
        pframe->refcount = 0;

        head_ptr = virt;
        head_ptr[0] = pslab_list;
        pslab_list = head_ptr;
        pslab_numfree += 1U;
    }

    spinlock_release_intr(&pslab_lock, intr_state);
}

static void init_pslab(void)
{
    for(size_t i = 0U; i < memmap->entry_count; ++i) {
        const struct limine_memmap_entry* entry = memmap->entries[i];

        if(entry->type == LIMINE_MEMMAP_USABLE) {
            uintptr_t start = page_align_up(entry->base);
            size_t length = align_floor(entry->length, PAGE_SIZE);

            if(start == 0U) {
                start += PAGE_SIZE;
                length -= PAGE_SIZE;
            }

            if(start + length <= PDMA_APPROX_END) {
                continue;
            }

            if(start < PDMA_APPROX_END) {
                length -= PDMA_APPROX_END + 1U - start;
                start = PDMA_APPROX_END + 1U;
            }

            if(length > 0U) {
                pslab_add_usable(phys_addr(start), length);
            }
        }
    }

    char hrsize_buffer[128];
    format_size_iec(hrsize_buffer, sizeof(hrsize_buffer), pslab_numpages * PAGE_SIZE);
    pr_verbose("pslab: tracking %s in %zu pages", hrsize_buffer, pslab_numpages);
}

initcall_bsp_pmm(init_pslab, INIT_MIDDLE);
