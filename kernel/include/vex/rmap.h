#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/types.h>

typedef struct pmap_s pmap_t;
typedef struct pframe_s pframe_t;

typedef struct rmap_entry_s rmap_entry_t;

struct rmap_entry_s {
    rmap_entry_t* next;
    pmap_t* pmap;
    virt_addr_t virt;
};

/// Maps the frame, which hands the new mapping one of the caller's references to it
/// @pre The caller holds the vmspace lock of the page table
int rmap_map(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t phys, unsigned vprot);

/// @pre The caller holds the vmspace lock of the page table
/// @post The caller runs tlb_shootdown, which this leaves out
/// @return The frame that was mapped, its mapping's reference now the caller's, or zero when nothing was
phys_addr_t rmap_unmap(pmap_t* restrict pmap, virt_addr_t virt) __nodiscard;

/// Moves the mapping at virt to another frame, the mapping's reference going with it
/// @pre The caller holds the vmspace lock of the page table
/// @post The caller runs tlb_shootdown, which this leaves out
/// @retval -EAGAIN A walker took the mapping of old_phys away meanwhile
int rmap_replace(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t old_phys, phys_addr_t new_phys, unsigned vprot);

/// @pre The caller holds the vmspace lock of the page table
/// @post The caller runs tlb_shootdown, which this leaves out
/// @retval -EAGAIN The page at virt is no longer the one at phys
int rmap_patch(pmap_t* restrict pmap, virt_addr_t virt, phys_addr_t phys, unsigned vprot);

/// @pre The caller holds the vmspace lock of the page table
/// @param sole Set when this mapping is the frame's only one and no file cache owns it, nullptr if unwanted
/// @return The frame mapped at virt with a new reference to it, or zero when nothing is mapped
phys_addr_t rmap_pin(pmap_t* restrict pmap, virt_addr_t virt, bool* restrict sole) __nodiscard;

/// Takes every mapping of the frame away, from any page table and without its vmspace lock
/// @post The caller runs tlb_shootdown, which this leaves out
/// @return The number of mappings removed, each of whose frame references is now the caller's
size_t rmap_unmap_all(pframe_t* restrict pframe) __nodiscard;

/// Makes every mapping of the frame read-only, from any page table and without its vmspace lock
/// @post The caller runs tlb_shootdown, which this leaves out
/// @return The number of writable mappings made read-only
size_t rmap_protect_all(pframe_t* restrict pframe) __nodiscard;

/// The most pages that rmap had mapped in the page table at one time
size_t rmap_peak_pages(const pmap_t* restrict pmap) __nodiscard;
