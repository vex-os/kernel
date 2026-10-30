#include <vex/assert.h>
#include <vex/commit.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/file.h>
#include <vex/hhdm.h>
#include <vex/initcall.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pframe.h>
#include <vex/pmap.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/rmap.h>
#include <vex/sched.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/tlb.h>
#include <vex/vms.h>
#include <vex/vnode.h>

#define FREE_BATCH 64

// Free address space a stack keeps under
// it to grow into, as stack_guard_gap in Linux
#define STACK_GUARD_PAGES (UINT64_C(256))
#define STACK_GUARD_BYTES (STACK_GUARD_PAGES * PAGE_SIZE)

typedef struct user_access_s user_access_t;

struct user_access_s {
    phys_addr_t pinned; // The frame to put_frame once done, or null
    bool device;        // device_regions_lock is held until then
};

static spinlock_t stale_lock = SPINLOCK_INIT;
static vm_region_t* stale_regions = nullptr;
static event_t stale_wakeup;

// Also held from resolving a device page to mapping it, and across a copy through
// that mapping, so vms_revoke finds the page mapped or comes before it is resolved
static spinlock_t device_regions_lock = SPINLOCK_INIT;
static vm_region_t* device_regions = nullptr;

static_assert(sizeof(vm_region_t) <= SLAB_MAX_SIZE);
static_assert(sizeof(vmspace_t) <= SLAB_MAX_SIZE);

static vm_region_t* find_region(vmspace_t* restrict vmspace, virt_addr_t virt)
{
    for(vm_region_t* it = vmspace->regions; it && address_less_equal(it->start, virt); it = it->next) {
        if(address_less(virt, it->end)) {
            return it;
        }
    }

    return nullptr;
}

static bool range_free(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    for(vm_region_t* it = vmspace->regions; it && address_less(it->start, end); it = it->next) {
        if(address_greater(it->end, start)) {
            return false;
        }
    }

    return true;
}

static void track_device_region_locked(vmspace_t* restrict vmspace, vm_region_t* restrict region)
{
    region->device_pmap = vmspace->pmap;
    region->device_next = device_regions;
    device_regions = region;
}

static void track_device_region(vmspace_t* restrict vmspace, vm_region_t* restrict region)
{
    if(region->device) {
        spinlock_acquire(&device_regions_lock);
        track_device_region_locked(vmspace, region);
        spinlock_release(&device_regions_lock);
    }
}

static void untrack_device_region(const vm_region_t* restrict region)
{
    spinlock_acquire(&device_regions_lock);

    vm_region_t** prev = &device_regions;

    while(*prev != region) {
        prev = &(*prev)->device_next;
    }

    *prev = region->device_next;

    spinlock_release(&device_regions_lock);
}

static void insert_region(vmspace_t* restrict vmspace, vm_region_t* restrict region)
{
    vm_region_t** prev = &vmspace->regions;

    while(*prev && address_less((*prev)->start, region->start)) {
        prev = &(*prev)->next;
    }

    region->next = *prev;
    *prev = region;

    track_device_region(vmspace, region);
}

static int split_region(vmspace_t* restrict vmspace, virt_addr_t virt)
{
    vm_region_t* region = find_region(vmspace, virt);

    if(region == nullptr || address_equal(region->start, virt)) {
        return 0;
    }

    vm_region_t* upper = slab_alloc(sizeof(vm_region_t));

    if(upper == nullptr) {
        return -ENOMEM;
    }

    upper->start = virt;
    upper->end = region->end;
    upper->vprot = region->vprot;
    upper->file = file_dupe(region->file);
    upper->file_offset = region->file_offset + address_diff(virt, region->start);
    upper->shared = region->shared;
    upper->may_write = region->may_write;
    upper->device = region->device;
    upper->noreserve = region->noreserve;
    upper->grows_down = region->grows_down; // Only the lowest part has the free gap under it to grow into
    upper->grow_floor = region->grow_floor;
    upper->next = region->next;

    // Under the lock, so vms_revoke finds the pages above virt in one region or the other
    if(region->device) {
        spinlock_acquire(&device_regions_lock);
    }

    region->end = virt;
    region->next = upper;

    if(region->device) {
        track_device_region_locked(vmspace, upper);
        spinlock_release(&device_regions_lock);
    }

    return 0;
}

static bool range_covered(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    virt_addr_t covered = start;

    for(vm_region_t* it = find_region(vmspace, start); it && address_less_equal(it->start, covered) && address_less(covered, end);
        it = it->next) {
        covered = it->end;
    }

    return address_greater_equal(covered, end);
}

// A range of user memory that begins and ends below the end of the user half of the address space
static bool user_range_ok(const vmspace_t* restrict vmspace, virt_addr_t start, size_t size)
{
    virt_addr_t user_end = pmap_user_end(vmspace->pmap);

    if(address_greater_equal(start, user_end)) {
        return false;
    }

    return size <= address_diff(user_end, start);
}

static bool range_valid(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    if(page_offset(start) || page_offset(end)) {
        return false;
    }

    return address_less(start, end) && address_less_equal(end, pmap_user_end(vmspace->pmap));
}

static void put_frame(phys_addr_t phys)
{
    if(pframe_decref(pframe_of(phys))) {
        pslab_free_phys(phys);
    }
}

// Private memory that a write can turn into pages of its own
static bool counts_commit(const vm_region_t* restrict region, unsigned vprot)
{
    return !region->device && !region->shared && !region->noreserve && (vprot & VPROT_WRITE);
}

static bool region_counted(const vm_region_t* restrict region)
{
    return counts_commit(region, region->vprot);
}

static size_t region_pages(const vm_region_t* restrict region)
{
    return page_count(address_diff(region->end, region->start));
}

static size_t counted_pages_in(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    size_t pages = 0U;

    for(const vm_region_t* it = vmspace->regions; it && address_less(it->start, end); it = it->next) {
        if(address_less_equal(it->end, start) || !region_counted(it)) {
            continue;
        }

        virt_addr_t overlap_start = address_max(it->start, start);
        virt_addr_t overlap_end = address_min(it->end, end);

        pages += page_count(address_diff(overlap_end, overlap_start));
    }

    return pages;
}

static void destroy_region(vm_region_t* restrict region)
{
    if(region->file) {
        file_close(region->file);
    }

    slab_free(region);
}

static void free_region(vm_region_t* restrict region, bool deferred)
{
    if(region_counted(region)) {
        commit_uncharge(region_pages(region));
    }

    if(region->file == nullptr || !deferred) {
        destroy_region(region);
        return;
    }

    bool intr_state = spinlock_acquire_intr(&stale_lock);
    region->next = stale_regions;
    stale_regions = region;
    spinlock_release_intr(&stale_lock, intr_state);

    event_trigger(&stale_wakeup, true);
}

static int unshare_locked(vmspace_t* restrict vmspace, const vm_region_t* restrict region, virt_addr_t page)
{
    unsigned vprot = region->vprot | VPROT_USER;
    bool sole;
    phys_addr_t phys = rmap_pin(vmspace->pmap, page, &sole);

    if(address_null(phys)) {
        return -EAGAIN;
    }

    if(sole) {
        int retval = 0;

        if((pmap_resolve_vprot(vmspace->pmap, page) & VPROT_WRITE) != (vprot & VPROT_WRITE)) {
            retval = pmap_patch_noflush(vmspace->pmap, page, vprot);
        }

        put_frame(phys);

        return retval;
    }

    phys_addr_t copy = pslab_alloc_phys();

    if(address_null(copy)) {
        put_frame(phys);
        return -ENOMEM;
    }

    void* target = phys_to_virt(copy);
    void* source = phys_to_virt(phys);
    memcpy(target, source, PAGE_SIZE);

    int retval = rmap_replace(vmspace->pmap, page, phys, copy, vprot);

    if(retval < 0) {
        pslab_free_phys(copy);
        put_frame(phys);
        return retval;
    }

    tlb_shootdown();

    put_frame(phys); // The old mapping's
    put_frame(phys); // The pin's

    return 0;
}

// Device pages have no pframe, so they stay out of the rmap, and no write faults are needed
// to track them dirty: they map with the region's vprot. The caller holds device_regions_lock
static int fault_device_locked(vmspace_t* restrict vmspace, const vm_region_t* restrict region, virt_addr_t page)
{
    if(address_valid(pmap_resolve_address(vmspace->pmap, page))) {
        return 0;
    }

    off_t offset = region->file_offset + address_diff(page, region->start);
    phys_addr_t phys;
    unsigned cache;
    int retval = vnode_resolve_page(region->file->node, offset, &phys, &cache);

    if(retval < 0) {
        return retval;
    }

    return pmap_map(vmspace->pmap, page, phys, region->vprot | (cache & VPROT_CACHE_MASK) | VPROT_USER);
}

static vm_region_t* region_above(vmspace_t* restrict vmspace, virt_addr_t virt)
{
    for(vm_region_t* it = vmspace->regions; it; it = it->next) {
        if(address_greater(it->start, virt)) {
            return it;
        }
    }

    return nullptr;
}

/// A stack grows down to the page that faulted under it, as Linux does, also when the fault skips pages
/// @return The stack, or nullptr when the page is past its floor, too close to another mapping, or over the commit limit
static vm_region_t* grow_stack_locked(vmspace_t* restrict vmspace, virt_addr_t page)
{
    vm_region_t* stack = region_above(vmspace, page);

    if(stack == nullptr || !stack->grows_down) {
        return nullptr;
    }

    if(address_less(page, stack->grow_floor)) {
        return nullptr;
    }

    virt_addr_t gap_start = virt_addr(0);

    if(page.value > STACK_GUARD_BYTES) {
        gap_start = address_sub(page, STACK_GUARD_BYTES);
    }

    if(!range_free(vmspace, gap_start, page)) {
        return nullptr;
    }

    size_t pages = page_count(address_diff(stack->start, page));

    if(region_counted(stack) && commit_charge(pages) < 0) {
        return nullptr;
    }

    stack->start = page;

    return stack;
}

static int fault_locked(vmspace_t* restrict vmspace, virt_addr_t page, unsigned access, bool exclusive)
{
    vm_region_t* region = find_region(vmspace, page);

    if(region == nullptr) {
        region = grow_stack_locked(vmspace, page);
    }

    if(region == nullptr) {
        return -EFAULT;
    }

    if((region->vprot & access) != access) {
        return -EACCES;
    }

    if(region->device) {
        spinlock_acquire(&device_regions_lock);
        int retval = fault_device_locked(vmspace, region, page);
        spinlock_release(&device_regions_lock);
        return retval;
    }

    // A private page of a file can take
    // two passes, the read on the first
    int kind = VMS_FAULT_MINOR;

    for(;;) {
        bool mapped = address_valid(pmap_resolve_address(vmspace->pmap, page));

        if(mapped && !exclusive) {
            return kind;
        }

        if(mapped && !region->shared) {
            int retval = unshare_locked(vmspace, region, page);

            if(retval < 0 && retval != -EAGAIN) {
                return retval;
            }

            if(retval == 0) {
                return kind;
            }

            continue;
        }

        if(region->file == nullptr) {
            break;
        }

        // A private page starts out as the file's own, and a write copies it on the next pass;
        // a shared one only becomes writable through a write fault, which marks it dirty
        off_t offset = region->file_offset + address_diff(page, region->start);
        unsigned vprot = region->vprot | VPROT_USER;

        if(!region->shared || !exclusive) {
            vprot &= ~VPROT_WRITE;
        }

        int retval = vnode_fault(region->file->node, offset, vmspace->pmap, page, vprot);

        if(retval < 0) {
            return retval;
        }

        if(retval == VMS_FAULT_MAJOR) {
            kind = VMS_FAULT_MAJOR;
        }

        if(region->shared) {
            return kind;
        }
    }

    phys_addr_t phys = pslab_alloc_phys();

    if(address_null(phys)) {
        return -ENOMEM;
    }

    void* data = phys_to_virt(phys);
    memset(data, 0, PAGE_SIZE);

    int retval = rmap_map(vmspace->pmap, page, phys, region->vprot | VPROT_USER);

    if(retval < 0) {
        pslab_free_phys(phys);
        return retval;
    }

    return VMS_FAULT_MINOR;
}

static phys_addr_t pin_locked(vmspace_t* restrict vmspace, virt_addr_t virt, unsigned access, bool exclusive)
{
    virt_addr_t page = page_align(virt);
    phys_addr_t phys = phys_addr(0);

    vm_region_t* region = find_region(vmspace, page);

    if(region && region->device) {
        return phys_addr(0); // No pframe to pin
    }

    while(address_null(phys)) {
        if(fault_locked(vmspace, page, access, exclusive) < 0) {
            return phys_addr(0);
        }

        phys = rmap_pin(vmspace->pmap, page, nullptr);
    }

    return phys;
}

static bool vms_current(const vmspace_t* restrict vmspace)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    return process && process->vmspace == vmspace;
}

// Gives where the byte at virt can be reached from the kernel until access_end, which a non-null
// result needs. Device memory is neither pinnable nor in the HHDM, so it is reached through the
// user's own mapping, and device_regions_lock keeps vms_revoke from taking it away meanwhile
static void* access_begin_locked(vmspace_t* restrict vmspace, virt_addr_t virt, unsigned access, bool exclusive,
    user_access_t* restrict out)
{
    vm_region_t* region = find_region(vmspace, virt);

    out->pinned = phys_addr(0);
    out->device = false;

    if(region && region->device) {
        // UNDONE: another address space isn't loaded, so its device memory needs a temporary
        // kernel mapping, and the driver's leave, as a read of MMIO registers may have side effects
        if(!vms_current(vmspace) || (region->vprot & access) != access) {
            return nullptr;
        }

        spinlock_acquire(&device_regions_lock);

        if(fault_device_locked(vmspace, region, page_align(virt)) < 0) {
            spinlock_release(&device_regions_lock);
            return nullptr;
        }

        out->device = true;

        return virt_addr_pointer(virt);
    }

    out->pinned = pin_locked(vmspace, virt, access, exclusive);

    if(address_null(out->pinned)) {
        return nullptr;
    }

    return (uint8_t*)(phys_to_virt(out->pinned)) + page_offset(virt);
}

static void access_end(const user_access_t* restrict user_access)
{
    if(user_access->device) {
        spinlock_release(&device_regions_lock);
    }

    if(address_valid(user_access->pinned)) {
        put_frame(user_access->pinned);
    }
}

static void free_batch(phys_addr_t* restrict batch, size_t* restrict count, bool shootdown)
{
    if(*count == 0) {
        return;
    }

    if(shootdown) {
        tlb_shootdown();
    }

    for(size_t i = 0; i < *count; ++i) {
        put_frame(batch[i]);
    }

    *count = 0;
}

static void release_device_pages(vmspace_t* restrict vmspace, const vm_region_t* restrict region, bool shootdown)
{
    untrack_device_region(region);

    bool unmapped = false;

    for(virt_addr_t page = region->start; address_less(page, region->end); page = address_add(page, PAGE_SIZE)) {
        if(0 == pmap_unmap_noflush(vmspace->pmap, page)) {
            unmapped = true;
        }
    }

    if(unmapped && shootdown) {
        tlb_shootdown();
    }
}

static void release_range(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, phys_addr_t* restrict batch,
    size_t* restrict count, bool shootdown)
{
    for(virt_addr_t page = start; address_less(page, end); page = address_add(page, PAGE_SIZE)) {
        phys_addr_t phys = rmap_unmap(vmspace->pmap, page);

        if(address_null(phys)) {
            continue;
        }

        batch[*count] = phys;
        *count += 1;

        if(*count == FREE_BATCH) {
            free_batch(batch, count, shootdown);
        }
    }
}

static void release_pages(vmspace_t* restrict vmspace, const vm_region_t* restrict region, phys_addr_t* restrict batch,
    size_t* restrict count, bool shootdown)
{
    if(region->device) {
        release_device_pages(vmspace, region, shootdown);
        return;
    }

    release_range(vmspace, region->start, region->end, batch, count, shootdown);
}

static void unmap_all_locked(vmspace_t* restrict vmspace, bool shootdown, bool deferred)
{
    phys_addr_t batch[FREE_BATCH];
    size_t count = 0;

    while(vmspace->regions) {
        vm_region_t* region = vmspace->regions;
        vmspace->regions = region->next;

        release_pages(vmspace, region, batch, &count, shootdown);
        free_region(region, deferred);
    }

    free_batch(batch, &count, shootdown);
}

// Holes in the range are fine, as with munmap
static int unmap_locked(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    int retval = split_region(vmspace, start);

    if(retval == 0) {
        retval = split_region(vmspace, end);
    }

    if(retval < 0) {
        return retval;
    }

    phys_addr_t batch[FREE_BATCH];
    size_t count = 0;

    vm_region_t** prev = &vmspace->regions;

    while(*prev && address_less((*prev)->start, end)) {
        vm_region_t* region = *prev;

        if(address_less(region->start, start)) {
            prev = &region->next;
            continue;
        }

        *prev = region->next;

        release_pages(vmspace, region, batch, &count, true);
        free_region(region, false);
    }

    free_batch(batch, &count, true);

    return 0;
}

static bool find_highest_gap(vmspace_t* restrict vmspace, size_t size, virt_addr_t* restrict out)
{
    virt_addr_t gap_start = virt_addr(VMS_MMAP_MIN);
    virt_addr_t top = virt_addr(VMS_MMAP_TOP);
    bool found = false;

    for(vm_region_t* it = vmspace->regions;; it = it->next) {
        virt_addr_t gap_end = top;

        if(it && address_less(it->start, top)) {
            gap_end = it->start;
        }

        if(address_greater(gap_end, gap_start) && address_diff(gap_end, gap_start) >= size) {
            *out = address_sub(gap_end, size);
            found = true;
        }

        if(it == nullptr || address_greater_equal(it->start, top)) {
            return found;
        }

        if(address_greater(it->end, gap_start)) {
            gap_start = it->end;
        }
    }
}

// Private writable memory takes its pages from the commit limit, and free_region gives them back.
// The replaced pages are the ones that the caller unmaps after this call
static vm_region_t* alloc_region(virt_addr_t start, virt_addr_t end, unsigned vprot, unsigned flags, const vm_backing_t* restrict backing,
    size_t replaced)
{
    vm_region_t* region = slab_alloc(sizeof(vm_region_t));

    if(region == nullptr) {
        return nullptr;
    }

    region->start = start;
    region->end = end;
    region->vprot = vprot & ~VPROT_USER;
    region->file = nullptr;
    region->file_offset = 0;
    region->shared = false;
    region->may_write = true;
    region->device = false;
    region->noreserve = flags & VMS_NORESERVE;

    if(backing && backing->file) {
        region->file = file_dupe(backing->file);
        region->file_offset = backing->file_offset;
        region->shared = backing->shared;
        region->may_write = backing->may_write;
        region->device = backing->device;
    }

    size_t pages = region_pages(region);

    if(region_counted(region) && commit_charge_replacing(pages, replaced) < 0) {
        destroy_region(region);
        return nullptr;
    }

    return region;
}

static void __noreturn release_thread(void* restrict arg)
{
    for(;;) {
        event_t* wakeup = &stale_wakeup;
        event_await(&wakeup, 1);

        bool intr_state = spinlock_acquire_intr(&stale_lock);
        vm_region_t* regions = stale_regions;
        stale_regions = nullptr;
        spinlock_release_intr(&stale_lock, intr_state);

        while(regions) {
            vm_region_t* next = regions->next;
            destroy_region(regions);
            regions = next;
        }
    }
}

vmspace_t* vms_create(void)
{
    vmspace_t* vmspace = slab_alloc(sizeof(vmspace_t));

    if(vmspace == nullptr) {
        return nullptr;
    }

    vmspace->pmap = pmap_create();

    if(vmspace->pmap == nullptr) {
        slab_free(vmspace);
        return nullptr;
    }

    vmspace->regions = nullptr;
    mutex_init(&vmspace->lock);

    return vmspace;
}

void vms_release(vmspace_t* restrict vmspace)
{
    // Released only once no hart has these
    // page tables loaded, so no TLB holds their entries
    unmap_all_locked(vmspace, false, true);
    pmap_release(vmspace->pmap);
    mutex_deinit(&vmspace->lock);
    slab_free(vmspace);
}

int vms_map(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, unsigned vprot, unsigned flags,
    const vm_backing_t* restrict backing)
{
    if(!range_valid(vmspace, start, end)) {
        return -EINVAL;
    }

    vm_region_t* region = alloc_region(start, end, vprot, flags, backing, 0U);

    if(region == nullptr) {
        return -ENOMEM;
    }

    mutex_acquire(&vmspace->lock);

    if(!range_free(vmspace, start, end)) {
        mutex_release(&vmspace->lock);
        free_region(region, false);
        return -EEXIST;
    }

    insert_region(vmspace, region);

    mutex_release(&vmspace->lock);

    return 0;
}

int vms_map_stack(vmspace_t* restrict vmspace, virt_addr_t floor, virt_addr_t start, virt_addr_t end, unsigned vprot)
{
    int retval = vms_map(vmspace, start, end, vprot, 0, nullptr);

    if(retval < 0) {
        return retval;
    }

    mutex_acquire(&vmspace->lock);

    vm_region_t* stack = find_region(vmspace, start);
    stack->grows_down = true;
    stack->grow_floor = floor;

    mutex_release(&vmspace->lock);

    return 0;
}

int vms_map_replace(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, unsigned vprot, unsigned flags,
    const vm_backing_t* restrict backing)
{
    if(!range_valid(vmspace, start, end)) {
        return -EINVAL;
    }

    mutex_acquire(&vmspace->lock);

    vm_region_t* region = alloc_region(start, end, vprot, flags, backing, counted_pages_in(vmspace, start, end));

    if(region == nullptr) {
        mutex_release(&vmspace->lock);
        return -ENOMEM;
    }

    int retval = unmap_locked(vmspace, start, end);

    if(retval < 0) {
        mutex_release(&vmspace->lock);
        free_region(region, false);
        return retval;
    }

    insert_region(vmspace, region);

    mutex_release(&vmspace->lock);

    return 0;
}

int vms_map_any(vmspace_t* restrict vmspace, size_t size, unsigned vprot, unsigned flags, const vm_backing_t* restrict backing,
    virt_addr_t* restrict out)
{
    if(size == 0 || page_offset(size)) {
        return -EINVAL;
    }

    vm_region_t* region = alloc_region(virt_addr(0), virt_addr(size), vprot, flags, backing, 0U);

    if(region == nullptr) {
        return -ENOMEM;
    }

    mutex_acquire(&vmspace->lock);

    if(!find_highest_gap(vmspace, size, &region->start)) {
        mutex_release(&vmspace->lock);
        free_region(region, false);
        return -ENOMEM;
    }

    region->end = address_add(region->start, size);
    insert_region(vmspace, region);

    *out = region->start;

    mutex_release(&vmspace->lock);

    return 0;
}

int vms_unmap(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    if(!range_valid(vmspace, start, end)) {
        return -EINVAL;
    }

    mutex_acquire(&vmspace->lock);
    int retval = unmap_locked(vmspace, start, end);
    mutex_release(&vmspace->lock);

    return retval;
}

int vms_protect(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, unsigned vprot)
{
    if(!range_valid(vmspace, start, end)) {
        return -EINVAL;
    }

    vprot &= ~VPROT_USER;

    mutex_acquire(&vmspace->lock);

    if(!range_covered(vmspace, start, end)) {
        mutex_release(&vmspace->lock);
        return -ENOMEM;
    }

    for(vm_region_t* it = find_region(vmspace, start); it && address_less(it->start, end); it = it->next) {
        if((vprot & VPROT_WRITE) && !it->may_write) {
            mutex_release(&vmspace->lock);
            return -EACCES;
        }
    }

    int retval = split_region(vmspace, start);

    if(retval == 0) {
        retval = split_region(vmspace, end);
    }

    if(retval < 0) {
        mutex_release(&vmspace->lock);
        return retval;
    }

    size_t gain = 0U;
    size_t loss = 0U;

    for(const vm_region_t* it = find_region(vmspace, start); it && address_less(it->start, end); it = it->next) {
        bool counted_before = region_counted(it);
        bool counted_after = counts_commit(it, vprot);

        if(!counted_before && counted_after) {
            gain += region_pages(it);
        }

        if(counted_before && !counted_after) {
            loss += region_pages(it);
        }
    }

    // The regions stay split when this fails, which changes nothing that a program sees
    if(gain && commit_charge(gain) < 0) {
        mutex_release(&vmspace->lock);
        return -ENOMEM;
    }

    bool patched = false;

    for(vm_region_t* it = find_region(vmspace, start); it && address_less(it->start, end); it = it->next) {
        it->vprot = vprot;

        for(virt_addr_t page = it->start; address_less(page, it->end); page = address_add(page, PAGE_SIZE)) {
            phys_addr_t phys = pmap_resolve_address(vmspace->pmap, page);

            if(address_null(phys)) {
                continue;
            }

            unsigned page_vprot = vprot | VPROT_USER;

            if(it->device) {
                page_vprot |= pmap_resolve_vprot(vmspace->pmap, page) & VPROT_CACHE_MASK;
            }
            else if(it->shared || pframe_shared(pframe_of(phys))) {
                page_vprot &= ~VPROT_WRITE;
            }

            if(0 == pmap_patch_noflush(vmspace->pmap, page, page_vprot)) {
                patched = true;
            }
        }
    }

    if(loss) {
        commit_uncharge(loss);
    }

    mutex_release(&vmspace->lock);

    if(patched) {
        tlb_shootdown();
    }

    return 0;
}

int vms_check_mapped(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    if(!range_valid(vmspace, start, end)) {
        return -EINVAL;
    }

    mutex_acquire(&vmspace->lock);
    bool covered = range_covered(vmspace, start, end);
    mutex_release(&vmspace->lock);

    if(!covered) {
        return -ENOMEM;
    }

    return 0;
}

int vms_discard(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    if(!range_valid(vmspace, start, end)) {
        return -EINVAL;
    }

    mutex_acquire(&vmspace->lock);

    if(!range_covered(vmspace, start, end)) {
        mutex_release(&vmspace->lock);
        return -ENOMEM;
    }

    for(vm_region_t* it = find_region(vmspace, start); it && address_less(it->start, end); it = it->next) {
        if(it->device) {
            mutex_release(&vmspace->lock);
            return -EINVAL;
        }
    }

    phys_addr_t batch[FREE_BATCH];
    size_t count = 0;

    for(vm_region_t* it = find_region(vmspace, start); it && address_less(it->start, end); it = it->next) {
        virt_addr_t clipped_start = address_max(start, it->start);
        virt_addr_t clipped_end = address_min(end, it->end);

        release_range(vmspace, clipped_start, clipped_end, batch, &count, true);
    }

    free_batch(batch, &count, true);
    mutex_release(&vmspace->lock);

    return 0;
}

int vms_sync(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end, bool wait)
{
    if(!range_valid(vmspace, start, end)) {
        return -EINVAL;
    }

    mutex_acquire(&vmspace->lock);
    bool covered = range_covered(vmspace, start, end);
    mutex_release(&vmspace->lock);

    if(!covered) {
        return -ENOMEM;
    }

    int retval = 0;
    virt_addr_t cursor = start;

    // One file at a time, as fsync sleeps on the filesystem
    while(wait && address_less(cursor, end)) {
        vnode_t* node = nullptr;

        mutex_acquire(&vmspace->lock);

        for(vm_region_t* it = vmspace->regions; it && address_less(it->start, end) && node == nullptr; it = it->next) {
            if(address_less_equal(it->end, cursor)) {
                continue;
            }

            cursor = it->end;

            if(it->shared && !it->device) {
                node = it->file->node;
                vnode_incref(node);
            }
        }

        mutex_release(&vmspace->lock);

        if(node == nullptr) {
            break;
        }

        int node_retval = vnode_fsync(node);
        vnode_decref(node);

        if(retval == 0) {
            retval = node_retval;
        }
    }

    return retval;
}

int vms_unmap_all(vmspace_t* restrict vmspace)
{
    mutex_acquire(&vmspace->lock);
    unmap_all_locked(vmspace, true, false);
    mutex_release(&vmspace->lock);
    return 0;
}

int vms_clone(vmspace_t* restrict vmspace, vmspace_t* restrict source)
{
    kassert(vmspace->regions == nullptr);

    mutex_acquire(&source->lock);

    bool patched = false;
    int retval = 0;

    vm_region_t** tail = &vmspace->regions;

    for(const vm_region_t* it = source->regions; it; it = it->next) {
        vm_backing_t backing;
        backing.file = it->file;
        backing.file_offset = it->file_offset;
        backing.shared = it->shared;
        backing.may_write = it->may_write;
        backing.device = it->device;

        unsigned flags = 0;

        if(it->noreserve) {
            flags |= VMS_NORESERVE;
        }

        vm_region_t* region = alloc_region(it->start, it->end, it->vprot, flags, &backing, 0U);

        if(region == nullptr) {
            retval = -ENOMEM;
            break;
        }

        region->grows_down = it->grows_down;
        region->grow_floor = it->grow_floor;
        region->next = nullptr;
        *tail = region;
        tail = &region->next;

        track_device_region(vmspace, region);

        if(it->device) {
            continue; // The child faults its device pages in itself
        }

        unsigned vprot = (it->vprot & ~VPROT_WRITE) | VPROT_USER;

        for(virt_addr_t page = it->start; address_less(page, it->end) && retval == 0; page = address_add(page, PAGE_SIZE)) {
            phys_addr_t phys = rmap_pin(source->pmap, page, nullptr);

            if(address_null(phys)) {
                continue;
            }

            retval = rmap_map(vmspace->pmap, page, phys, vprot);

            if(retval < 0) {
                put_frame(phys);
                break;
            }

            // Fails only when a walker took the page meanwhile
            if((it->vprot & VPROT_WRITE) && !it->shared && 0 == pmap_patch_noflush(source->pmap, page, vprot)) {
                patched = true;
            }
        }

        if(retval < 0) {
            break;
        }
    }

    if(patched) {
        tlb_shootdown();
    }

    mutex_release(&source->lock);

    if(retval < 0) {
        vms_unmap_all(vmspace);
    }

    return retval;
}

int vms_fault(vmspace_t* restrict vmspace, virt_addr_t virt, unsigned access)
{
    if(address_greater_equal(virt, pmap_user_end(vmspace->pmap))) {
        return -EFAULT;
    }

    mutex_acquire(&vmspace->lock);
    access &= ~VPROT_USER;
    virt_addr_t page = page_align(virt);

    int retval = fault_locked(vmspace, page, access, (access & VPROT_WRITE));
    mutex_release(&vmspace->lock);

    return retval;
}

void vms_populate(vmspace_t* restrict vmspace, virt_addr_t start, virt_addr_t end)
{
    for(virt_addr_t page = start; address_less(page, end); page = address_add(page, PAGE_SIZE)) {
        if(vms_fault(vmspace, page, VPROT_NONE) < 0) {
            return;
        }
    }
}

void vms_revoke(vnode_t* restrict node)
{
    bool unmapped = false;

    spinlock_acquire(&device_regions_lock);

    for(vm_region_t* it = device_regions; it; it = it->device_next) {
        if(it->file->node != node) {
            continue;
        }

        for(virt_addr_t page = it->start; address_less(page, it->end); page = address_add(page, PAGE_SIZE)) {
            if(0 == pmap_unmap_noflush(it->device_pmap, page)) {
                unmapped = true;
            }
        }
    }

    spinlock_release(&device_regions_lock);

    if(unmapped) {
        tlb_shootdown();
    }
}

phys_addr_t vms_touch(vmspace_t* restrict vmspace, virt_addr_t virt)
{
    if(address_greater_equal(virt, pmap_user_end(vmspace->pmap))) {
        return phys_addr(0);
    }

    mutex_acquire(&vmspace->lock);

    phys_addr_t phys = pin_locked(vmspace, virt, VPROT_NONE, true);

    if(address_valid(phys)) {
        put_frame(phys);
        phys = address_add(phys, page_offset(virt));
    }

    mutex_release(&vmspace->lock);

    return phys;
}

phys_addr_t vms_pin(vmspace_t* restrict vmspace, virt_addr_t virt)
{
    if(address_greater_equal(virt, pmap_user_end(vmspace->pmap))) {
        return phys_addr(0);
    }

    mutex_acquire(&vmspace->lock);

    vm_region_t* region = find_region(vmspace, virt);
    phys_addr_t phys = phys_addr(0);

    if(region) {
        phys = pin_locked(vmspace, virt, VPROT_READ, !region->shared);
    }

    mutex_release(&vmspace->lock);

    if(address_valid(phys)) {
        phys = address_add(phys, page_offset(virt));
    }

    return phys;
}

void vms_unpin(phys_addr_t phys)
{
    put_frame(page_align(phys));
}

ssize_t vms_copyin(void* restrict dst, vmspace_t* restrict vmspace, virt_addr_t src, size_t size)
{
    uint8_t* ptr = dst;

    if(!user_range_ok(vmspace, src, size)) {
        return -EFAULT;
    }

    mutex_acquire(&vmspace->lock);

    while(size) {
        user_access_t user_access;
        const uint8_t* page = access_begin_locked(vmspace, src, VPROT_READ, false, &user_access);

        if(page == nullptr) {
            mutex_release(&vmspace->lock);
            return -EFAULT;
        }

        size_t chunk = PAGE_SIZE - page_offset(src);

        if(chunk > size) {
            chunk = size;
        }

        memcpy(ptr, page, chunk);

        access_end(&user_access);

        src = address_add(src, chunk);
        ptr += chunk;
        size -= chunk;
    }

    mutex_release(&vmspace->lock);

    return ptr - (uint8_t*)(dst);
}

ssize_t vms_copyin_str(char* restrict dst, vmspace_t* restrict vmspace, virt_addr_t src, size_t max_sz)
{
    if(max_sz == 0) {
        return -EINVAL;
    }

    char* ptr = dst;
    size_t remaining = max_sz;

    mutex_acquire(&vmspace->lock);

    while(remaining) {
        user_access_t user_access;
        const char* page = nullptr;

        if(address_less(src, pmap_user_end(vmspace->pmap))) {
            page = access_begin_locked(vmspace, src, VPROT_READ, false, &user_access);
        }

        if(page == nullptr) {
            mutex_release(&vmspace->lock);
            return -EFAULT;
        }

        size_t chunk = PAGE_SIZE - page_offset(src);

        if(chunk > remaining) {
            chunk = remaining;
        }

        const char* null_pos = memchr(page, 0x00, chunk);

        if(null_pos) {
            size_t len = null_pos - page + 1;

            memcpy(ptr, page, len);

            access_end(&user_access);

            mutex_release(&vmspace->lock);

            return ptr - dst + len - 1;
        }

        memcpy(ptr, page, chunk);

        access_end(&user_access);

        src = address_add(src, chunk);
        ptr += chunk;
        remaining -= chunk;
    }

    mutex_release(&vmspace->lock);

    dst[max_sz - 1] = 0x00;

    return -ENAMETOOLONG;
}

ssize_t vms_copyout(vmspace_t* restrict vmspace, virt_addr_t dst, const void* restrict src, size_t size)
{
    const uint8_t* ptr = src;

    if(!user_range_ok(vmspace, dst, size)) {
        return -EFAULT;
    }

    mutex_acquire(&vmspace->lock);

    while(size) {
        user_access_t user_access;
        uint8_t* page = access_begin_locked(vmspace, dst, VPROT_WRITE, true, &user_access);

        if(page == nullptr) {
            mutex_release(&vmspace->lock);
            return -EFAULT;
        }

        size_t chunk = PAGE_SIZE - page_offset(dst);

        if(chunk > size) {
            chunk = size;
        }

        memcpy(page, ptr, chunk);

        access_end(&user_access);

        dst = address_add(dst, chunk);
        ptr += chunk;
        size -= chunk;
    }

    mutex_release(&vmspace->lock);

    return ptr - (const uint8_t*)(src);
}

ssize_t vms_copyout_str(vmspace_t* restrict vmspace, virt_addr_t dst, const char* restrict src, size_t max_sz)
{
    if(max_sz == 0) {
        return -EINVAL;
    }

    size_t len = strnlen(src, max_sz);

    if(len < max_sz) {
        return vms_copyout(vmspace, dst, src, len + 1);
    }

    return -ENAMETOOLONG;
}

static void init_vms(void)
{
    event_init(&stale_wakeup);

    if(thread_spawn_kernel(&release_thread, nullptr) == nullptr) {
        panic("vms: out of memory");
    }
}

initcall_bsp_vfs(init_vms, INIT_FIRST);
