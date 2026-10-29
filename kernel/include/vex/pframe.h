#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/types.h>

typedef struct fcache_s fcache_t;
typedef struct rmap_entry_s rmap_entry_t;

#define PFRAME_PSLAB    0x0001 // Managed by pslab
#define PFRAME_CACHE    0x0002 // Owned by a file cache, owner and index are valid
#define PFRAME_UPTODATE 0x0004 // Holds the file contents, zeroes past the end of file
#define PFRAME_DIRTY    0x0008
#define PFRAME_ACCESSED 0x0010 // Used since reclaim last looked at it

#define PFRAME_DIRTY_SHIFT 24 // Bits 24-31: dirty sectors of a file cache page, the first one lowest

typedef struct pframe_s pframe_t;

struct pframe_s {
    uint32_t refcount;
    uint32_t flags;     // PFRAME; the file cache bits are under owner->lock
    fcache_t* owner;    // Under owner->lock
    uint64_t index;     // Page index within the owner
    pframe_t* lru_prev; // Under the file cache LRU lock
    pframe_t* lru_next;
    rmap_entry_t* mappings; // Under the rmap lock of the frame
};

pframe_t* pframe_of(phys_addr_t phys) __nodiscard;
phys_addr_t pframe_phys(const pframe_t* restrict pframe) __nodiscard;

void pframe_incref(pframe_t* restrict pframe);
bool pframe_decref(pframe_t* restrict pframe) __nodiscard;
bool pframe_shared(const pframe_t* restrict pframe) __nodiscard;

void pframe_storage(phys_addr_t* restrict start, size_t* restrict length);
