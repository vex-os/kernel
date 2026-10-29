#include <vex/assert.h>
#include <vex/errno.h>
#include <vex/hart.h>
#include <vex/hhdm.h>
#include <vex/initcall.h>
#include <vex/kbase.h>
#include <vex/kprintf.h>
#include <vex/linker.h>
#include <vex/memmap.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pmap.h>
#include <vex/pslab.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/tlb.h>

static_assert(sizeof(pmap_t) <= SLAB_MAX_SIZE);

pmap_t sys_pmap;

static unsigned max_leaf_level = 1;

static size_t pte_index(virt_addr_t virt, unsigned level)
{
    return (virt.value >> pmentry_level_shift(level)) & PMENTRY_LEVEL_MASK;
}

static pmentry_t* pmentry_get(pmentry_t* restrict table, size_t index, bool allocate)
{
    if(pmentry_valid(table[index])) {
        return phys_to_virt(pmentry_table_address(table[index]));
    }

    if(allocate) {
        phys_addr_t phys = pslab_alloc_phys();

        if(address_valid(phys)) {
            pmentry_t* child = phys_to_virt(phys);
            memset(child, 0, PAGE_SIZE);
            table[index] = pmentry_table(phys);
            return child;
        }
    }

    return nullptr;
}

static pmentry_t* pmentry_walk(pmap_t* restrict pmap, virt_addr_t virt, unsigned leaf_level, bool allocate, unsigned* restrict out_level)
{
    pmentry_t* table = pmap_root(pmap, virt);
    unsigned level = pmap->num_levels;

    while(level > leaf_level) {
        size_t index = pte_index(virt, level);

        if(pmentry_valid(table[index]) && pmentry_leaf(table[index], level)) {
            break;
        }

        table = pmentry_get(table, index, allocate);

        if(table == nullptr) {
            if(out_level) {
                *out_level = level; // Where the table is missing, so callers can skip what it would cover
            }

            return nullptr;
        }

        level -= 1;
    }

    if(out_level) {
        *out_level = level;
    }

    return &table[pte_index(virt, level)];
}

static int split_large_nolock(pmentry_t* restrict entry, unsigned level)
{
    phys_addr_t phys = pslab_alloc_phys();

    if(address_null(phys)) {
        return -ENOMEM;
    }

    pmentry_t* table = phys_to_virt(phys);
    phys_addr_t base = pmentry_address(*entry, level);
    uintptr_t step = pmentry_level_size(level - 1);
    unsigned vprot = pmentry_vprot(*entry, level);

    for(size_t i = 0; i < PAGEMAP_SIZE; ++i) {
        table[i] = pmentry_setup(address_add(base, i * step), vprot, level - 1);
    }

    *entry = pmentry_table(phys);

    return 0;
}

static int pmentry_walk_page_nolock(pmap_t* restrict pmap, virt_addr_t virt, pmentry_t** restrict out)
{
    for(;;) {
        unsigned level;
        pmentry_t* entry = pmentry_walk(pmap, virt, 1, false, &level);

        if(entry == nullptr || !pmentry_valid(*entry)) {
            return -ENOENT;
        }

        if(level == 1) {
            *out = entry;
            return 0;
        }

        int retval = split_large_nolock(entry, level);

        if(retval < 0) {
            return retval;
        }
    }
}

static unsigned pick_leaf_level(virt_addr_t virt, phys_addr_t phys, size_t size, unsigned vprot)
{
    if(vprot & VPROT_USER) {
        return 1;
    }

    for(unsigned level = max_leaf_level; level > 1; level -= 1) {
        uintptr_t mask = pmentry_level_size(level) - 1;

        if(0 == ((virt.value | phys.value) & mask) && size > mask) {
            return level;
        }
    }

    return 1;
}

static void pmap_table_free(pmentry_t* restrict table, size_t begin, size_t end, unsigned depth)
{
    if(depth) {
        for(size_t i = begin; i < end; ++i) {
            if(pmentry_valid(table[i]) && !pmentry_leaf(table[i], depth + 1)) {
                pmentry_t* child = phys_to_virt(pmentry_table_address(table[i]));
                pmap_table_free(child, 0, PAGEMAP_SIZE, depth - 1);
                pslab_free_phys(virt_to_phys(child));
            }
        }
    }
}

static phys_addr_t pmap_resolve_address_nolock(pmap_t* restrict pmap, virt_addr_t virt)
{
    unsigned level;
    pmentry_t* entry = pmentry_walk(pmap, virt, 1, false, &level);

    if(entry && pmentry_valid(*entry)) {
        return address_add(pmentry_address(*entry, level), virt.value & (pmentry_level_size(level) - 1));
    }

    return phys_addr(0);
}

static int map_kernel_section(const void* restrict start, const void* restrict end, unsigned vprot)
{
    virt_addr_t virt = page_align(virt_addr_from_pointer(start));
    virt_addr_t virt_end = page_align_up(virt_addr_from_pointer(end));
    phys_addr_t phys = phys_addr(virt.value - kernel_base_virt + kernel_base_phys);

    return pmap_map_range(&sys_pmap, virt, phys, address_diff(virt_end, virt), vprot);
}

static void map_hhdm_run(uintptr_t start, uintptr_t end)
{
    if(end > start && pmap_map_range(&sys_pmap, virt_addr(start + hhdm_base), phys_addr(start), end - start, VPROT_RW) < 0) {
        panic("mmu: failed to map %#zx-%#zx into the HHDM", start, end);
    }
}

static void map_hhdm(void)
{
    uintptr_t run_start = 0;
    uintptr_t run_end = 0;

    for(size_t i = 0; i < memmap->entry_count; ++i) {
        const struct limine_memmap_entry* entry = memmap->entries[i];

        if(!memmap_is_ram_type(entry->type)) {
            continue;
        }

        uintptr_t start = page_align(entry->base);
        uintptr_t end = page_align_up(entry->base + entry->length);

        kassert(start >= run_start); // Limine sorts the memory map by base

        if(start > run_end) {
            map_hhdm_run(run_start, run_end);
            run_start = start;
        }

        if(end > run_end) {
            run_end = end;
        }
    }

    map_hhdm_run(run_start, run_end);
}

pmap_t* pmap_create(void)
{
    pmap_t* pmap = slab_alloc(sizeof(pmap_t));

    if(pmap) {
        pmap->lock = SPINLOCK_INIT;
        pmap->num_levels = sys_pmap.num_levels;

        if(arch_pmap_alloc(pmap) == 0) {
            arch_pmap_kcopy(pmap, &sys_pmap);
            return pmap;
        }

        slab_free(pmap);
    }

    return nullptr;
}

void pmap_release(pmap_t* restrict pmap)
{
    pmap_table_free(pmap_root(pmap, virt_addr(0)), 0, PAGEMAP_USER_END, pmap->num_levels - 1);
    arch_pmap_free(pmap);
    slab_free(pmap);
}

int pmap_map(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t phys, unsigned vprot)
{
    bool intr_state = spinlock_acquire_intr(&pmap->lock);

    pmentry_t* entry = pmentry_walk(pmap, virt, 1, true, nullptr);
    int retval;

    if(entry == nullptr) {
        retval = -ENOMEM;
    }
    else if(pmentry_valid(*entry)) {
        retval = -EEXIST;
    }
    else {
        *entry = pmentry_setup(page_align(phys), vprot, 1);
        pmap_flush(virt);
        retval = 0;
    }

    spinlock_release_intr(&pmap->lock, intr_state);

    return retval;
}

int pmap_map_range(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t phys, size_t size, unsigned vprot)
{
    kassert(0 == ((virt.value | phys.value | size) & (PAGE_SIZE - 1)));

    bool intr_state = spinlock_acquire_intr(&pmap->lock);

    int retval = 0;

    while(size > 0) {
        unsigned level = pick_leaf_level(virt, phys, size, vprot);
        pmentry_t* entry = pmentry_walk(pmap, virt, level, true, nullptr);

        if(entry == nullptr) {
            retval = -ENOMEM;
            break;
        }

        if(pmentry_valid(*entry)) {
            retval = -EEXIST;
            break;
        }

        *entry = pmentry_setup(phys, vprot, level);
        pmap_flush(virt);

        uintptr_t step = pmentry_level_size(level);
        virt = address_add(virt, step);
        phys = address_add(phys, step);
        size -= step;
    }

    spinlock_release_intr(&pmap->lock, intr_state);

    return retval;
}

int pmap_patch_noflush(pmap_t* restrict pmap, virt_addr_t virt, unsigned vprot)
{
    bool intr_state = spinlock_acquire_intr(&pmap->lock);

    pmentry_t* entry;
    int retval = pmentry_walk_page_nolock(pmap, virt, &entry);

    if(retval == -ENOENT) {
        retval = -EINVAL;
    }

    if(retval == 0) {
        *entry = pmentry_setup(pmentry_address(*entry, 1), vprot, 1);
        pmap_flush(virt);
    }

    spinlock_release_intr(&pmap->lock, intr_state);

    return retval;
}

int pmap_patch(pmap_t* restrict pmap, virt_addr_t virt, unsigned vprot)
{
    int retval = pmap_patch_noflush(pmap, virt, vprot);

    if(retval == 0) {
        tlb_shootdown();
    }

    return retval;
}

int pmap_unmap_noflush(pmap_t* restrict pmap, virt_addr_t virt)
{
    bool intr_state = spinlock_acquire_intr(&pmap->lock);

    pmentry_t* entry;
    int retval = pmentry_walk_page_nolock(pmap, virt, &entry);

    if(retval == 0) {
        *entry = PMENTRY_NULL;
        pmap_flush(virt);
    }

    spinlock_release_intr(&pmap->lock, intr_state);

    return retval;
}

int pmap_unmap(pmap_t* restrict pmap, virt_addr_t virt)
{
    int retval = pmap_unmap_noflush(pmap, virt);

    if(retval == 0) {
        tlb_shootdown();
    }

    return retval;
}

int pmap_unmap_range_noflush(pmap_t* restrict pmap, virt_addr_t virt, size_t size)
{
    kassert(0 == ((virt.value | size) & (PAGE_SIZE - 1)));

    bool intr_state = spinlock_acquire_intr(&pmap->lock);

    virt_addr_t end = address_add(virt, size);
    int retval = 0;

    while(address_less(virt, end)) {
        unsigned level;
        pmentry_t* entry = pmentry_walk(pmap, virt, 1, false, &level);
        uintptr_t mask = pmentry_level_size(level) - 1;

        if(entry == nullptr || !pmentry_valid(*entry)) {
            virt = virt_addr((virt.value | mask) + 1);
            continue;
        }

        if(0 == (virt.value & mask) && address_diff(end, virt) > mask) {
            *entry = PMENTRY_NULL;
            pmap_flush(virt);
            virt = address_add(virt, mask + 1);
            continue;
        }

        retval = split_large_nolock(entry, level);

        if(retval < 0) {
            break;
        }
    }

    spinlock_release_intr(&pmap->lock, intr_state);

    return retval;
}

phys_addr_t pmap_resolve_address(pmap_t* restrict pmap, virt_addr_t virt)
{
    bool intr_state = spinlock_acquire_intr(&pmap->lock);
    phys_addr_t result = pmap_resolve_address_nolock(pmap, virt);
    spinlock_release_intr(&pmap->lock, intr_state);
    return result;
}

unsigned pmap_resolve_vprot(pmap_t* restrict pmap, virt_addr_t virt)
{
    bool intr_state = spinlock_acquire_intr(&pmap->lock);

    unsigned level;
    pmentry_t* entry = pmentry_walk(pmap, virt, 1, false, &level);
    unsigned result = 0;

    if(entry && pmentry_valid(*entry)) {
        result = pmentry_vprot(*entry, level);
    }

    spinlock_release_intr(&pmap->lock, intr_state);

    return result;
}

static void init_mmu(void)
{
    sys_pmap.lock = SPINLOCK_INIT;
    sys_pmap.num_levels = pmap_detect_levels();
    max_leaf_level = pmap_detect_leaf_levels();

    int retval = arch_pmap_alloc(&sys_pmap);

    if(retval < 0) {
        panic("mmu: out of memory");
    }

    retval = map_kernel_section(&text_start, &text_end, VPROT_RX);

    if(retval < 0) {
        panic("mmu: failed to map kernel.text");
    }

    retval = map_kernel_section(&rodata_start, &rodata_end, VPROT_READ);

    if(retval < 0) {
        panic("mmu: failed to map kernel.rodata");
    }

    retval = map_kernel_section(&data_start, &data_end, VPROT_RW);

    if(retval < 0) {
        panic("mmu: failed to map kernel.data");
    }

    map_hhdm();

    // Address spaces copy the kernel half's top-level entries when they are created,
    // so every one of them must exist before the first copy for the kernel half to stay shared
    for(size_t i = PAGEMAP_USER_END; i < PAGEMAP_SIZE; ++i) {
        if(!pmentry_get(sys_pmap.virt, i, true)) {
            panic("mmu: out of memory");
        }
    }

    pmap_switch(&sys_pmap);
}

initcall_bsp_mmu(init_mmu, INIT_MIDDLE);
