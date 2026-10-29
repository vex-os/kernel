#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/hhdm.h>
#include <vex/hrsize.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/memmap.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pdma.h>
#include <vex/pframe.h>
#include <vex/string.h>

// Indexed by frame number up
// to the highest usable frame
static pframe_t* pframes = nullptr;
static size_t num_pframes = 0U;

static phys_addr_t storage_start = {};
static size_t storage_length = 0U;

pframe_t* pframe_of(phys_addr_t phys)
{
    size_t index = phys.value / PAGE_SIZE;
    kassert(index < num_pframes);
    return &pframes[index];
}

phys_addr_t pframe_phys(const pframe_t* restrict pframe)
{
    return phys_addr((uintptr_t)(pframe - pframes) * PAGE_SIZE);
}

void pframe_incref(pframe_t* restrict pframe)
{
    atomic_fetch_add(&pframe->refcount, 1, ATOMIC_RELAXED);
}

bool pframe_decref(pframe_t* restrict pframe)
{
    uint32_t refcount = atomic_load(&pframe->refcount, ATOMIC_ACQUIRE);

    while(refcount > 1) {
        if(atomic_cmpxchg_weak(&pframe->refcount, &refcount, refcount - 1, ATOMIC_ACQ_REL, ATOMIC_ACQUIRE)) {
            return false;
        }
    }

    return true;
}

bool pframe_shared(const pframe_t* restrict pframe)
{
    return atomic_load(&pframe->refcount, ATOMIC_ACQUIRE) > 1;
}

void pframe_storage(phys_addr_t* restrict start, size_t* restrict length)
{
    *start = storage_start;
    *length = storage_length;
}

static void init_pframe(void)
{
    uintptr_t usable_end = 0U;

    for(size_t i = 0U; i < memmap->entry_count; ++i) {
        const struct limine_memmap_entry* entry = memmap->entries[i];
        uintptr_t end = page_align(entry->base + entry->length);

        if(entry->type == LIMINE_MEMMAP_USABLE && end > usable_end) {
            usable_end = end;
        }
    }

    num_pframes = usable_end / PAGE_SIZE;
    storage_length = page_align_up(num_pframes * sizeof(pframe_t));

    // Taken from above the DMA zone, so pdma keeps all of it
    for(size_t i = 0U; i < memmap->entry_count; ++i) {
        const struct limine_memmap_entry* entry = memmap->entries[i];

        if(entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uintptr_t start = page_align_up(entry->base);
        uintptr_t end = page_align(entry->base + entry->length);

        if(start <= PDMA_APPROX_END) {
            start = PDMA_APPROX_END + 1U;
        }

        if(end > start && end - start >= storage_length) {
            storage_start = phys_addr(start);
            break;
        }
    }

    if(address_null(storage_start)) {
        panic("pframe: no room for %zu frames", num_pframes);
        unreachable();
    }

    pframes = phys_to_virt(storage_start);
    memset(pframes, 0, storage_length);

    char hrsize_buffer[128];
    format_size_iec(hrsize_buffer, sizeof(hrsize_buffer), storage_length);
    pr_verbose("pframe: %zu frames described in %s at %p", num_pframes, hrsize_buffer, (void*)(storage_start.value));
}

initcall_bsp_pmm(init_pframe, INIT_EARLY);
