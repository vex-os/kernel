#include <vex/bitmap.h>
#include <vex/hhdm.h>
#include <vex/hrsize.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/limits.h>
#include <vex/memmap.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pdma.h>
#include <vex/spinlock.h>

typedef struct memmap_info_s memmap_info_t;

struct memmap_info_s {
    uintptr_t start;
    uintptr_t end;
    size_t length;
    size_t npages;
};

static bitmap_t* pdma_bitmap = nullptr;
static size_t pdma_numpages = 0U;
static size_t pdma_numused = 0U;
static size_t pdma_lastpage = 0U;
static spinlock_t pdma_lock = SPINLOCK_INIT;

static void get_memmap_info(const struct limine_memmap_entry* restrict entry, memmap_info_t* restrict info)
{
    info->start = align_ceil(entry->base, PAGE_SIZE);
    info->length = align_floor(entry->length, PAGE_SIZE);

    if(info->start == 0U) {
        info->start += PAGE_SIZE;
        info->length -= PAGE_SIZE;
    }

    info->end = info->start + info->length - 1U;
    info->npages = page_count(info->length);
}

static void add_region(phys_addr_t start, size_t length)
{
    size_t page = start.value / PAGE_SIZE;
    size_t npages = page_count(length);

    bitmap_range_set(pdma_bitmap, page, page + npages - 1U);

    pr_verbose("pdma: adding region: start=%p end=%p", (void*)(start.value), (void*)(address_add(start, length - 1U).value));
}

static size_t find_free_range(size_t first, size_t last, size_t num_pages)
{
    for(size_t page = first; page <= last; page += 1U) {
        if(bitmap_range_isset(pdma_bitmap, page, page + num_pages - 1U)) {
            return page;
        }
    }

    return SIZE_MAX;
}

phys_addr_t pdma_alloc_phys(size_t num_pages)
{
    if(num_pages == 0U || num_pages > pdma_numpages) {
        return phys_addr(0U);
    }

    bool intr_state = spinlock_acquire_intr(&pdma_lock);

    size_t last_start = pdma_numpages - num_pages;
    size_t hint = pdma_lastpage <= last_start ? pdma_lastpage : 0U;
    size_t page = find_free_range(hint, last_start, num_pages);

    if(page == SIZE_MAX && hint > 0U) {
        page = find_free_range(0U, hint - 1U, num_pages);
    }

    if(page == SIZE_MAX) {
        spinlock_release_intr(&pdma_lock, intr_state);
        return phys_addr(0U);
    }

    bitmap_range_clear(pdma_bitmap, page, page + num_pages - 1U);
    pdma_lastpage = page + num_pages;
    pdma_numused += num_pages;

    spinlock_release_intr(&pdma_lock, intr_state);

    return phys_addr(page * PAGE_SIZE);
}

void* pdma_alloc_virt(size_t num_pages)
{
    phys_addr_t address = pdma_alloc_phys(num_pages);
    if(address_null(address))
        return nullptr;
    return phys_to_virt(address);
}

void pdma_free_phys(phys_addr_t phys, size_t num_pages)
{
    size_t page = phys.value / PAGE_SIZE;

    bool intr_state = spinlock_acquire_intr(&pdma_lock);

    if(address_valid(phys) && num_pages) {
        bitmap_range_set(pdma_bitmap, page, page + num_pages - 1U);
        pdma_numused -= num_pages;
        pdma_lastpage = page;
    }

    spinlock_release_intr(&pdma_lock, intr_state);
}

void pdma_free_virt(void* restrict virt, size_t num_pages)
{
    if(virt) {
        phys_addr_t phys = virt_to_phys(virt);

        pdma_free_phys(phys, num_pages);
    }
}

static void init_pdma(void)
{
    size_t bitmap_npages = 0U;

    for(size_t i = 0U; i < memmap->entry_count; ++i) {
        const struct limine_memmap_entry* entry = memmap->entries[i];

        if(entry->type == LIMINE_MEMMAP_USABLE) {
            memmap_info_t info;
            get_memmap_info(entry, &info);

            if(info.start < PDMA_APPROX_END) {
                if(info.end >= PDMA_APPROX_END) {
                    info.end = PDMA_APPROX_END;
                    info.length = info.end - info.start + 1U;
                    info.npages = page_count(info.length);
                }

                if((info.end / PAGE_SIZE + 1U) > bitmap_npages) {
                    bitmap_npages = info.end / PAGE_SIZE + 1U;
                }
            }
        }
    }

    pdma_numpages = align_ceil(bitmap_npages, BITMAP_CHUNK_BITS);

    size_t bitmap_size = bitmap_bytecount(pdma_numpages);
    bitmap_npages = page_count(bitmap_size);
    bitmap_size = bitmap_npages * PAGE_SIZE;

    uintptr_t bitmap_phys = 0U;

    for(size_t i = 0U; i < memmap->entry_count; ++i) {
        const struct limine_memmap_entry* entry = memmap->entries[i];

        if(entry->type == LIMINE_MEMMAP_USABLE) {
            memmap_info_t info;
            get_memmap_info(entry, &info);

            if(info.npages >= bitmap_npages && info.start + bitmap_size <= PDMA_APPROX_END) {
                bitmap_phys = info.start;
                pdma_bitmap = phys_to_virt(phys_addr(bitmap_phys));
                add_region(phys_addr(bitmap_phys), bitmap_size);
                break;
            }
        }
    }

    if(pdma_bitmap == nullptr) {
        panic("pdma: out of memory");
    }

    bitmap_range_clear(pdma_bitmap, 0U, pdma_numpages - 1U);

    for(size_t i = 0U; i < memmap->entry_count; ++i) {
        const struct limine_memmap_entry* entry = memmap->entries[i];

        if(entry->type == LIMINE_MEMMAP_USABLE) {
            memmap_info_t info;
            get_memmap_info(entry, &info);

            if(info.start >= PDMA_APPROX_END)
                continue;

            if(info.end >= PDMA_APPROX_END) {
                info.end = PDMA_APPROX_END;
                info.length = info.end - info.start + 1U;
            }

            if(info.start == bitmap_phys) {
                info.start += bitmap_size;
                info.length -= bitmap_size;
            }

            if(info.length > 0U) {
                add_region(phys_addr(info.start), info.length);
            }
        }
    }

    char hrsize_buffer[128];
    format_size_iec(hrsize_buffer, sizeof(hrsize_buffer), pdma_numpages * PAGE_SIZE);
    pr_verbose("pdma: tracking %s in %zu pages", hrsize_buffer, pdma_numpages);
}

initcall_bsp_pmm(init_pdma, INIT_FIRST);
