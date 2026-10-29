#include <vex/assert.h>
#include <vex/errno.h>
#include <vex/kmalloc.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pmap.h>
#include <vex/pslab.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/sprintf.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/vprot.h>

#define MALLOC_ALIGNMENT sizeof(uintptr_t)

#define BLOCK_TYPE_FREE 0x0000U
#define BLOCK_TYPE_USED 0x0001U

typedef struct memblock_s memblock_t;

struct memblock_s {
    memblock_t* prev;
    memblock_t* next;
    unsigned short type;
    virt_addr_t address;
    size_t length;
};

static_assert(sizeof(memblock_t) <= SLAB_MAX_SIZE);

static memblock_t* head = nullptr;
static memblock_t* tail = nullptr;
static virt_addr_t heap_end = { .value = HEAP_REGION_BASE };
static spinlock_t heap_lock = SPINLOCK_INIT;

static memblock_t* find_free_block(size_t size)
{
    memblock_t* it;

    for(it = head; it; it = it->next) {
        if(it->type == BLOCK_TYPE_FREE && it->length >= size) {
            return it;
        }
    }

    return nullptr;
}

static memblock_t* extend_heap(size_t size)
{
    size_t i;
    phys_addr_t phys;
    virt_addr_t virt;

    size_t npages = page_count(size);

    virt_addr_t region_end = virt_addr(HEAP_REGION_BASE + HEAP_REGION_SIZE);

    if(npages > address_diff(region_end, heap_end) / PAGE_SIZE) {
        return nullptr;
    }

    memblock_t* block = slab_alloc(sizeof(memblock_t));

    if(block == nullptr) {
        return nullptr;
    }

    block->address = heap_end;
    block->length = npages * PAGE_SIZE;
    block->type = BLOCK_TYPE_FREE;
    block->prev = tail;
    block->next = nullptr;

    for(i = 0; i < npages; ++i) {
        phys = pslab_alloc_phys();

        if(address_null(phys)) {
            goto error_cleanup;
        }

        virt = address_add(block->address, i * PAGE_SIZE);
        int retval = pmap_map(&sys_pmap, virt, phys, VPROT_RW);

        if(retval < 0) {
            pslab_free_phys(phys);
            goto error_cleanup;
        }
    }

    heap_end = address_add(heap_end, block->length);

    if(tail) {
        tail->next = block;
        tail = block;
    }
    else {
        head = block;
        tail = block;
    }

    return block;

error_cleanup:
    while(i--) {
        virt = address_add(block->address, i * PAGE_SIZE);
        pslab_free_phys(pmap_resolve_address(&sys_pmap, virt));
        pmap_unmap_noflush(&sys_pmap, virt); // Mapped a moment ago and never touched, under heap_lock
    }

    slab_free(block);

    return nullptr;
}

static void split_block(memblock_t* restrict block, size_t size)
{
    size_t aligned_sz = align_ceil(size, MALLOC_ALIGNMENT);
    size_t remainder = block->length - aligned_sz;

    if(remainder < 4 * sizeof(uintptr_t)) {
        return;
    }

    memblock_t* new_block = slab_alloc(sizeof(memblock_t));

    if(new_block == nullptr) {
        return;
    }

    new_block->address = address_add(block->address, aligned_sz);
    new_block->length = remainder;
    new_block->type = BLOCK_TYPE_FREE;
    new_block->prev = block;
    new_block->next = block->next;

    if(block->next) {
        block->next->prev = new_block;
    }
    else {
        tail = new_block;
    }

    block->next = new_block;
    block->length = aligned_sz;
}

static memblock_t* find_block_at(const void* restrict pointer)
{
    virt_addr_t address = virt_addr_from_pointer(pointer);
    memblock_t* it;

    for(it = head; it; it = it->next) {
        if(address_equal(it->address, address)) {
            return it;
        }
    }

    return nullptr;
}

static void coalesce(memblock_t* restrict block)
{
    memblock_t* next_block;
    memblock_t* prev_block;

    if(block->next && block->next->type == BLOCK_TYPE_FREE) {
        next_block = block->next;

        block->length += next_block->length;
        block->next = next_block->next;

        if(next_block->next) {
            next_block->next->prev = block;
        }
        else {
            tail = block;
        }

        slab_free(next_block);
    }

    if(block->prev && block->prev->type == BLOCK_TYPE_FREE) {
        prev_block = block->prev;

        prev_block->length += block->length;
        prev_block->next = block->next;

        if(block->next) {
            block->next->prev = prev_block;
        }
        else {
            tail = prev_block;
        }

        slab_free(block);
    }
}

void* kmalloc(size_t size)
{
    if(size == 0) {
        return nullptr;
    }

    bool intr_state = spinlock_acquire_intr(&heap_lock);

    size_t aligned_sz = align_ceil(size, MALLOC_ALIGNMENT);
    memblock_t* block = find_free_block(aligned_sz);

    if(block == nullptr) {
        block = extend_heap(aligned_sz);

        if(block == nullptr) {
            spinlock_release_intr(&heap_lock, intr_state);

            return nullptr;
        }
    }

    split_block(block, aligned_sz);

    block->type = BLOCK_TYPE_USED;

    spinlock_release_intr(&heap_lock, intr_state);

    return memset(virt_addr_pointer(block->address), 0, size);
}

void* krealloc(void* restrict ptr, size_t size)
{
    if(ptr == nullptr) {
        return kmalloc(size);
    }

    if(size == 0) {
        kfree(ptr);

        return nullptr;
    }

    size_t aligned_sz = align_ceil(size, MALLOC_ALIGNMENT);
    size_t old_length;

    bool intr_state = spinlock_acquire_intr(&heap_lock);

    memblock_t* block = find_block_at(ptr);

    if(block == nullptr) {
        spinlock_release_intr(&heap_lock, intr_state);
        return nullptr;
    }

    if(block->length >= aligned_sz) {
        spinlock_release_intr(&heap_lock, intr_state);
        return ptr;
    }

    old_length = block->length;

    spinlock_release_intr(&heap_lock, intr_state);

    void* new_ptr = kmalloc(aligned_sz);

    if(new_ptr == nullptr) {
        return nullptr;
    }

    memcpy(new_ptr, ptr, old_length);
    kfree(ptr);

    return new_ptr;
}

void kfree(void* restrict ptr)
{
    if(ptr == nullptr) {
        return;
    }

    bool intr_state = spinlock_acquire_intr(&heap_lock);

    memblock_t* block = find_block_at(ptr);

    if(block == nullptr || block->type == BLOCK_TYPE_FREE) {
        spinlock_release_intr(&heap_lock, intr_state);
        panic("kfree: invalid or double free of %p", ptr);
    }

    block->type = BLOCK_TYPE_FREE;

    coalesce(block);

    spinlock_release_intr(&heap_lock, intr_state);
}

char* kstrdup(const char* restrict string)
{
    if(string == nullptr) {
        return nullptr;
    }

    size_t size = 1 + strlen(string);
    char* result = kmalloc(size);

    if(result == nullptr) {
        return nullptr;
    }

    memset(result, 0, size);
    kstrncpy(result, string, size - 1);

    return result;
}

char* kasprintf(const char* restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char* result = kvasprintf(fmt, ap);
    va_end(ap);

    return result;
}

char* kvasprintf(const char* restrict fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int length = vsnprintf(nullptr, 0, fmt, ap);

    if(length < 0) {
        va_end(ap_copy);
        return nullptr;
    }

    char* result = kmalloc(length + 1);

    if(result == nullptr) {
        va_end(ap_copy);
        return nullptr;
    }

    memset(result, 0, length + 1);
    vsnprintf(result, length + 1, fmt, ap_copy);

    va_end(ap_copy);

    return result;
}
