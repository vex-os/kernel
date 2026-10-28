#pragma once

#include <vex/compiler.h>
#include <vex/spinlock.h>
#include <vex/types.h>
#include <vex/vprot.h>

#define AMD64_PML_ADDRESS_MASK UINT64_C(0x000FFFFFFFFFF000)

#define AMD64_PML_PRESENT UINT64_C(0x0000000000000001)
#define AMD64_PML_WRITE   UINT64_C(0x0000000000000002)
#define AMD64_PML_USER    UINT64_C(0x0000000000000004)
#define AMD64_PML_PWT     UINT64_C(0x0000000000000008)
#define AMD64_PML_PCD     UINT64_C(0x0000000000000010)
#define AMD64_PML_PAT     UINT64_C(0x0000000000000080) // In a 4 KiB entry
#define AMD64_PML_LARGE   UINT64_C(0x0000000000000080) // PS, in a 2 MiB or 1 GiB entry
#define AMD64_PML_PATL    UINT64_C(0x0000000000001000) // PAT, in a 2 MiB or 1 GiB entry
#define AMD64_PML_NOEXEC  UINT64_C(0x8000000000000000)

// Software bit of a user page that keeps its
// frame but allows no access: PRESENT is clear,
// as an x86 page that is present is always readable
#define AMD64_PML_PROTNONE UINT64_C(0x0000000000000200)

#define AMD64_PAGEMAP_START_KERN 256U

// Level 1 entries map 4 KiB pages; each level above covers 512 times more
#define PMENTRY_LVL1_SHIFT UINT64_C(12)
#define PMENTRY_LEVEL_BITS UINT64_C(9)
#define PMENTRY_LEVEL_MASK UINT64_C(0x1FF)

#define PMENTRY_NULL UINT64_C(0x0000000000000000)

#define MMIO_REGION_BASE   UINT64_C(0xFFFFFE0000000000)
#define MMIO_REGION_SIZE   UINT64_C(0x0000008000000000)
#define HEAP_REGION_BASE   UINT64_C(0xFFFFFE8000000000)
#define HEAP_REGION_SIZE   UINT64_C(0x0000008000000000)
#define KSTACK_REGION_BASE UINT64_C(0xFFFFFF0000000000)
#define KSTACK_REGION_SIZE UINT64_C(0x0000008000000000)

#define PAGEMAP_SIZE     512U
#define PAGEMAP_USER_END AMD64_PAGEMAP_START_KERN

// PAT entries 0-3 keep their power-on types
// so PWT and PCD alone still mean what firmware
// and the bootloader expect; entry 4 adds write-combining
#define AMD64_PAT_VALUE UINT64_C(0x0007040100070406)

typedef struct pmap_s pmap_t;

typedef uint64_t pmentry_t;

struct pmap_s {
    spinlock_t lock;
    unsigned num_levels;
    pmentry_t* virt;
    uintptr_t phys;
};

unsigned pmap_detect_levels(void);
unsigned pmap_detect_leaf_levels(void);

int arch_pmap_alloc(pmap_t* restrict pmap) __nodiscard;
void arch_pmap_free(pmap_t* restrict pmap);
void arch_pmap_zero(pmap_t* restrict pmap);
void arch_pmap_kcopy(pmap_t* restrict dest, const pmap_t* restrict source);

static __always_inline __nodiscard pmentry_t* pmap_root(const pmap_t* restrict pmap, uintptr_t virt)
{
    return pmap->virt;
}

static __always_inline __nodiscard uintptr_t pmap_user_end(const pmap_t* restrict pmap)
{
    return UINT64_C(1) << (PMENTRY_LVL1_SHIFT + PMENTRY_LEVEL_BITS * pmap->num_levels - 1);
}

static __always_inline __nodiscard unsigned pmentry_level_shift(unsigned level)
{
    return PMENTRY_LVL1_SHIFT + PMENTRY_LEVEL_BITS * (level - 1);
}

static __always_inline __nodiscard uintptr_t pmentry_level_size(unsigned level)
{
    return UINT64_C(1) << pmentry_level_shift(level);
}

static __always_inline __nodiscard pmentry_t amd64_pat_bit(unsigned level)
{
    if(level > 1)
        return AMD64_PML_PATL;
    return AMD64_PML_PAT;
}

static __always_inline __nodiscard pmentry_t amd64_pat_bits(unsigned vprot, unsigned level)
{
    switch(vprot & VPROT_CACHE_MASK) {
        case VPROT_CACHE_WT:
            return AMD64_PML_PWT;
        case VPROT_CACHE_UC:
            return AMD64_PML_PCD | AMD64_PML_PWT;
        case VPROT_CACHE_WC:
            return amd64_pat_bit(level);
        default:
            return 0;
    }
}

static __always_inline __nodiscard unsigned amd64_pat_vprot(pmentry_t entry, unsigned level)
{
    static const unsigned types[8] = {
        VPROT_CACHE_WB,
        VPROT_CACHE_WT,
        VPROT_CACHE_UC, // UC-, which MTRRs may turn into write-combining
        VPROT_CACHE_UC,
        VPROT_CACHE_WC,
        VPROT_CACHE_WT,
        VPROT_CACHE_UC,
        VPROT_CACHE_UC,
    };

    unsigned index = 0;

    if(entry & AMD64_PML_PWT)
        index |= 1;
    if(entry & AMD64_PML_PCD)
        index |= 2;
    if(entry & amd64_pat_bit(level))
        index |= 4;

    return types[index];
}

static __always_inline __nodiscard bool pmentry_valid(pmentry_t entry)
{
    return entry & (AMD64_PML_PRESENT | AMD64_PML_PROTNONE);
}

static __always_inline __nodiscard bool pmentry_leaf(pmentry_t entry, unsigned level)
{
    return level == 1 || (entry & AMD64_PML_LARGE);
}

static __always_inline __nodiscard uintptr_t pmentry_address(pmentry_t entry, unsigned level)
{
    return entry & AMD64_PML_ADDRESS_MASK & ~(pmentry_level_size(level) - 1);
}

static __always_inline __nodiscard uintptr_t pmentry_table_address(pmentry_t entry)
{
    return entry & AMD64_PML_ADDRESS_MASK;
}

static __always_inline __nodiscard unsigned pmentry_vprot(pmentry_t entry, unsigned level)
{
    unsigned vprot = VPROT_NONE;
    if(entry & AMD64_PML_PRESENT)
        vprot |= VPROT_READ;
    if(entry & AMD64_PML_WRITE)
        vprot |= VPROT_WRITE;
    if(entry & AMD64_PML_USER)
        vprot |= VPROT_USER;
    if(0 == (entry & AMD64_PML_NOEXEC))
        vprot |= VPROT_EXEC;
    vprot |= amd64_pat_vprot(entry, level);
    return vprot;
}

static __always_inline __nodiscard pmentry_t pmentry_setup(uintptr_t address, unsigned vprot, unsigned level)
{
    pmentry_t result = PMENTRY_NULL;

    result |= address & AMD64_PML_ADDRESS_MASK;
    result |= AMD64_PML_NOEXEC;

    if((vprot & VPROT_USER) && 0 == (vprot & (VPROT_READ | VPROT_WRITE | VPROT_EXEC))) {
        result |= AMD64_PML_PROTNONE;
    }
    else {
        result |= AMD64_PML_PRESENT;
    }

    if(level > 1) {
        result |= AMD64_PML_LARGE;
    }

    if(vprot & VPROT_WRITE)
        result |= AMD64_PML_WRITE;
    if(vprot & VPROT_USER)
        result |= AMD64_PML_USER;
    if(vprot & VPROT_EXEC)
        result &= ~AMD64_PML_NOEXEC;
    result |= amd64_pat_bits(vprot, level);

    return result;
}

static __always_inline __nodiscard pmentry_t pmentry_table(uintptr_t address)
{
    return (address & AMD64_PML_ADDRESS_MASK) | AMD64_PML_PRESENT | AMD64_PML_WRITE | AMD64_PML_USER;
}

static __always_inline void pmap_switch(const pmap_t* restrict pmap)
{
    asm volatile("movq %0, %%cr3" ::"r"(pmap->phys) : "memory");
}

static __always_inline void pmap_flush_all(void)
{
    uint64_t cr3;
    asm volatile("movq %%cr3, %0" : "=r"(cr3)::"memory");
    asm volatile("movq %0, %%cr3" ::"r"(cr3) : "memory");
}

static __always_inline void pmap_flush(uintptr_t virt)
{
    asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}
