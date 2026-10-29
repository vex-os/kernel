#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/fcache.h>
#include <vex/hart.h>
#include <vex/hhdm.h>
#include <vex/initcall.h>
#include <vex/mount.h>
#include <vex/page.h>
#include <vex/panic.h>
#include <vex/pframe.h>
#include <vex/pslab.h>
#include <vex/rmap.h>
#include <vex/sched.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/tlb.h>
#include <vex/vnode.h>
#include <vex/vprot.h>

#define GATHER_BATCH 16

static spinlock_t lru_lock = SPINLOCK_INIT;
static pframe_t* lru_head = nullptr;
static pframe_t* lru_tail = nullptr;
static size_t lru_length = 0;

static spinlock_t dirty_lock = SPINLOCK_INIT;
static vnode_t* dirty_head = nullptr;

static size_t dirty_pages = 0;

static event_t writeback_wakeup;
static bool writeback_kicked = false;

static void lru_append(pframe_t* restrict pframe)
{
    pframe->lru_prev = lru_tail;
    pframe->lru_next = nullptr;

    if(lru_tail) {
        lru_tail->lru_next = pframe;
    }
    else {
        lru_head = pframe;
    }

    lru_tail = pframe;
    lru_length += 1;
}

static void lru_unlink(pframe_t* restrict pframe)
{
    if(pframe->lru_prev) {
        pframe->lru_prev->lru_next = pframe->lru_next;
    }
    else {
        lru_head = pframe->lru_next;
    }

    if(pframe->lru_next) {
        pframe->lru_next->lru_prev = pframe->lru_prev;
    }
    else {
        lru_tail = pframe->lru_prev;
    }

    pframe->lru_prev = nullptr;
    pframe->lru_next = nullptr;
    lru_length -= 1;
}

static void* page_data(const pframe_t* restrict pframe)
{
    return phys_to_virt(pframe_phys(pframe));
}

static size_t valid_bytes(uint64_t index, off_t size)
{
    uint64_t start = index * PAGE_SIZE;
    uint64_t end = size;

    if(end <= start) {
        return 0;
    }

    if(end - start >= PAGE_SIZE) {
        return PAGE_SIZE;
    }

    return end - start;
}

static void queue_dirty(vnode_t* restrict node)
{
    fcache_t* cache = &node->pages;

    spinlock_acquire(&dirty_lock);

    if(!cache->queued) {
        vnode_incref(node);
        cache->queued = true;
        cache->dirty_next = dirty_head;
        dirty_head = node;
    }

    spinlock_release(&dirty_lock);
}

static void mark_dirty(vnode_t* restrict node, pframe_t* restrict pframe, unsigned sectors)
{
    fcache_t* cache = &node->pages;

    pframe->flags |= sectors << PFRAME_DIRTY_SHIFT;

    if(pframe->flags & PFRAME_DIRTY) {
        return;
    }

    pframe->flags |= PFRAME_DIRTY;
    cache->num_dirty += 1;
    atomic_fetch_add(&dirty_pages, 1, ATOMIC_RELAXED);

    queue_dirty(node);
}

static void mark_clean(fcache_t* restrict cache, pframe_t* restrict pframe)
{
    if(0 == (pframe->flags & PFRAME_DIRTY)) {
        return;
    }

    pframe->flags &= ~(PFRAME_DIRTY | (UINT32_MAX << PFRAME_DIRTY_SHIFT));
    cache->num_dirty -= 1;
    atomic_fetch_sub(&dirty_pages, 1, ATOMIC_RELAXED);
}

static pframe_t* add_page(fcache_t* restrict cache, uint64_t index, phys_addr_t phys)
{
    void* data = phys_to_virt(phys);

    memset(data, 0, PAGE_SIZE);

    pframe_t* pframe = pframe_of(phys);
    pframe->owner = cache;
    pframe->index = index;
    pframe->flags |= PFRAME_CACHE;

    if(radix_insert(&cache->tree, index, pframe) < 0) {
        pframe->flags &= ~PFRAME_CACHE;
        pframe->owner = nullptr;
        pslab_free_phys(phys);
        return nullptr;
    }

    cache->num_pages += 1;

    if(cache->memory) {
        pframe->flags |= PFRAME_UPTODATE;
        return pframe;
    }

    spinlock_acquire(&lru_lock);
    lru_append(pframe);
    spinlock_release(&lru_lock);

    return pframe;
}

static pframe_t* get_page(fcache_t* restrict cache, uint64_t index)
{
    pframe_t* pframe = radix_lookup(&cache->tree, index);

    if(pframe) {
        return pframe;
    }

    mutex_release(&cache->lock);
    phys_addr_t phys = pslab_alloc_phys();
    mutex_acquire(&cache->lock);

    if(address_null(phys)) {
        return nullptr;
    }

    pframe = radix_lookup(&cache->tree, index);

    if(pframe) {
        pslab_free_phys(phys);
        return pframe;
    }

    return add_page(cache, index, phys);
}

static void remove_page(fcache_t* restrict cache, pframe_t* restrict pframe)
{
    if(!cache->memory) {
        spinlock_acquire(&lru_lock);
        lru_unlink(pframe);
        spinlock_release(&lru_lock);
    }

    mark_clean(cache, pframe);

    pframe_t* removed = radix_remove(&cache->tree, pframe->index);
    kassert(removed == pframe);

    cache->num_pages -= 1;

    pframe->flags &= ~(PFRAME_CACHE | PFRAME_UPTODATE | PFRAME_ACCESSED);
    pframe->owner = nullptr;
    pframe->index = 0;

    size_t mappings = rmap_unmap_all(pframe);

    if(mappings) {
        tlb_shootdown();

        for(size_t i = 0; i < mappings; ++i) {
            bool last = pframe_decref(pframe);
            kassert(!last); // The cache still holds its own
        }
    }

    if(pframe_decref(pframe)) {
        pslab_free_phys(pframe_phys(pframe));
    }
}

static bool sector_size_valid(const block_interface_t* restrict volume)
{
    return volume->sector_size && volume->sector_size <= PAGE_SIZE && 0 == (PAGE_SIZE % volume->sector_size);
}

static unsigned sector_span(vnode_t* restrict node, size_t in_page, size_t length)
{
    size_t sector_size = node->fs->volume->sector_size;
    size_t first = in_page / sector_size;
    size_t last = (in_page + length - 1) / sector_size;

    return ((2U << last) - 1) & ~((1U << first) - 1);
}

static int fill_page(vnode_t* restrict node, fcache_bmap_t bmap, pframe_t* restrict pframe, off_t size)
{
    const block_interface_t* volume = node->fs->volume;
    size_t sector_size = volume->sector_size;
    uint8_t* data = page_data(pframe);

    size_t valid = valid_bytes(pframe->index, size);
    size_t num_sectors = (valid + sector_size - 1) / sector_size;
    uint64_t first = pframe->index * (PAGE_SIZE / sector_size);

    memset(data, 0, PAGE_SIZE);

    for(size_t i = 0; i < num_sectors;) {
        fcache_extent_t extent;
        int retval = bmap(node, first + i, false, &extent);

        if(retval < 0) {
            return retval;
        }

        if(extent.count == 0) {
            return -EIO;
        }

        size_t count = extent.count;

        if(count > num_sectors - i) {
            count = num_sectors - i;
        }

        if(extent.mapped) {
            retval = block_read(volume, extent.lba, count, data + i * sector_size);

            if(retval < 0) {
                return retval;
            }
        }

        i += count;
    }

    // The last sector may run past the end of file
    memset(data + valid, 0, PAGE_SIZE - valid);

    pframe->flags |= PFRAME_UPTODATE;

    return 0;
}

static int allocate_span(vnode_t* restrict node, fcache_bmap_t bmap, uint64_t pos, size_t length)
{
    size_t sector_size = node->fs->volume->sector_size;
    uint64_t sector = pos / sector_size;
    uint64_t end = (pos + length + sector_size - 1) / sector_size;

    while(sector < end) {
        fcache_extent_t extent;
        int retval = bmap(node, sector, true, &extent);

        if(retval < 0) {
            return retval;
        }

        if(extent.count == 0 || !extent.mapped) {
            return -EIO;
        }

        sector += extent.count;
    }

    return 0;
}

static int write_run(vnode_t* restrict node, fcache_bmap_t bmap, uint64_t sector, size_t count, const uint8_t* restrict data)
{
    const block_interface_t* volume = node->fs->volume;

    while(count > 0) {
        fcache_extent_t extent;
        int retval = bmap(node, sector, false, &extent);

        if(retval < 0) {
            return retval;
        }

        if(extent.count == 0) {
            return -EIO;
        }

        size_t chunk = extent.count;

        if(chunk > count) {
            chunk = count;
        }

        // A hole stays a hole; the sectors
        // written to were allocated by fcache_write
        if(extent.mapped) {
            retval = block_write(volume, extent.lba, chunk, data);

            if(retval < 0) {
                return retval;
            }
        }

        sector += chunk;
        count -= chunk;
        data += chunk * volume->sector_size;
    }

    return 0;
}

static int write_page(vnode_t* restrict node, fcache_bmap_t bmap, pframe_t* restrict pframe, off_t size)
{
    size_t sector_size = node->fs->volume->sector_size;
    const uint8_t* data = page_data(pframe);
    unsigned dirty = pframe->flags >> PFRAME_DIRTY_SHIFT;

    size_t valid = valid_bytes(pframe->index, size);
    size_t num_sectors = (valid + sector_size - 1) / sector_size;
    uint64_t first = pframe->index * (PAGE_SIZE / sector_size);

    for(size_t i = 0; i < num_sectors;) {
        if(0 == (dirty & (1U << i))) {
            i += 1;
            continue;
        }

        size_t run = 1;

        while(i + run < num_sectors && (dirty & (1U << (i + run)))) {
            run += 1;
        }

        int retval = write_run(node, bmap, first + i, run, data + i * sector_size);

        if(retval < 0) {
            return retval;
        }

        i += run;
    }

    return 0;
}

static int writeback_locked(vnode_t* restrict node, fcache_bmap_t bmap, off_t size)
{
    fcache_t* cache = &node->pages;
    int retval = 0;

    void* batch[GATHER_BATCH];
    uint64_t indices[GATHER_BATCH];
    uint64_t start = 0;
    size_t count;

    while(cache->num_dirty > 0 && (count = radix_gather(&cache->tree, start, batch, indices, GATHER_BATCH)) > 0) {
        for(size_t i = 0; i < count; ++i) {
            pframe_t* pframe = batch[i];

            if(0 == (pframe->flags & PFRAME_DIRTY)) {
                continue;
            }

            if(rmap_protect_all(pframe)) {
                tlb_shootdown();
            }

            int page_retval = write_page(node, bmap, pframe, size);

            if(page_retval < 0) {
                if(retval == 0) {
                    retval = page_retval;
                }

                continue;
            }

            mark_clean(cache, pframe);
        }

        if(indices[count - 1] == UINT64_MAX) {
            break;
        }

        start = indices[count - 1] + 1;
    }

    return retval;
}

static bool over_dirty_limit(void)
{
    size_t dirtyable_count = atomic_load(&pslab_numfree, ATOMIC_RELAXED) + atomic_load(&lru_length, ATOMIC_RELAXED);
    size_t dirtyable_percent = dirtyable_count / 100 * CONFIG_FCACHE_DIRTY_PERCENT;
    return atomic_load(&dirty_pages, ATOMIC_RELAXED) > dirtyable_percent;
}

static void kick_writeback(void)
{
    if(!atomic_exchange(&writeback_kicked, true, ATOMIC_ACQ_REL)) {
        event_trigger(&writeback_wakeup, true);
    }
}

static void __noreturn writeback_thread(void* restrict arg)
{
    for(;;) {
        event_t* wakeup = &writeback_wakeup;
        event_await_timeout(&wakeup, 1, UINT64_C(1000000) * CONFIG_FCACHE_WRITEBACK_MSEC);

        atomic_store(&writeback_kicked, false, ATOMIC_RELEASE);

        vfs_sync_all();
    }
}

static int dev_bmap(vnode_t* restrict node, uint64_t sector, bool create, fcache_extent_t* restrict out)
{
    const block_interface_t* volume = node->fs->volume;

    if(sector >= volume->sector_count) {
        return -EIO;
    }

    out->lba = sector;
    out->count = volume->sector_count - sector;
    out->mapped = true;

    return 0;
}

static off_t dev_size(const vnode_t* restrict node)
{
    const block_interface_t* volume = node->fs->volume;
    return volume->sector_count * volume->sector_size;
}

static int dev_fsync(vnode_t* restrict node)
{
    return fcache_writeback(node, &dev_bmap, dev_size(node));
}

static const vnode_ops_t dev_ops = {
    .fsync = &dev_fsync,
};

void fcache_init(fcache_t* restrict cache)
{
    mutex_init(&cache->lock);
    radix_init(&cache->tree);
    cache->num_pages = 0;
    cache->num_dirty = 0;
    cache->memory = false;
    cache->queued = false;
    cache->dirty_next = nullptr;
}

void fcache_deinit(fcache_t* restrict cache)
{
    kassert(!cache->queued);

    mutex_acquire(&cache->lock);

    void* batch[GATHER_BATCH];
    size_t count;

    while((count = radix_gather(&cache->tree, 0, batch, nullptr, GATHER_BATCH)) > 0) {
        for(size_t i = 0; i < count; ++i) {
            remove_page(cache, batch[i]);
        }
    }

    mutex_release(&cache->lock);

    radix_deinit(&cache->tree);
    mutex_deinit(&cache->lock);
}

void fcache_set_memory(fcache_t* restrict cache)
{
    kassert(cache->num_pages == 0);

    cache->memory = true;
}

ssize_t fcache_read(vnode_t* restrict node, fcache_bmap_t bmap, void* restrict buffer, size_t count, off_t offset, off_t file_size)
{
    fcache_t* cache = &node->pages;

    if(offset < 0) {
        return -EINVAL;
    }

    if(!cache->memory && !sector_size_valid(node->fs->volume)) {
        return -EINVAL;
    }

    if(offset >= file_size) {
        return 0;
    }

    if(count > (uint64_t)(file_size - offset)) {
        count = file_size - offset;
    }

    size_t done = 0;
    int retval = 0;

    mutex_acquire(&cache->lock);

    while(done < count) {
        uint64_t pos = offset + done;
        uint64_t index = pos / PAGE_SIZE;
        size_t in_page = pos % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - in_page;

        if(chunk > count - done) {
            chunk = count - done;
        }

        pframe_t* pframe = nullptr;

        if(cache->memory) {
            pframe = radix_lookup(&cache->tree, index);

            if(pframe == nullptr) {
                memset((uint8_t*)(buffer) + done, 0, chunk);
                done += chunk;
                continue;
            }
        }
        else {
            pframe = get_page(cache, index);
        }

        if(pframe == nullptr) {
            retval = -ENOMEM;
            break;
        }

        if(0 == (pframe->flags & PFRAME_UPTODATE)) {
            retval = fill_page(node, bmap, pframe, file_size);

            if(retval < 0) {
                remove_page(cache, pframe);
                break;
            }
        }

        pframe->flags |= PFRAME_ACCESSED;

        const uint8_t* data = page_data(pframe);
        memcpy((uint8_t*)(buffer) + done, data + in_page, chunk);

        done += chunk;
    }

    mutex_release(&cache->lock);

    if(done == 0 && retval < 0) {
        return retval;
    }

    return done;
}

ssize_t fcache_write(vnode_t* restrict node, fcache_bmap_t bmap, const void* restrict buffer, size_t count, off_t offset, off_t file_size)
{
    fcache_t* cache = &node->pages;

    if(offset < 0) {
        return -EINVAL;
    }

    if(!cache->memory && !sector_size_valid(node->fs->volume)) {
        return -EINVAL;
    }

    size_t done = 0;
    int retval = 0;

    mutex_acquire(&cache->lock);

    while(done < count) {
        uint64_t pos = offset + done;
        uint64_t index = pos / PAGE_SIZE;
        size_t in_page = pos % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - in_page;

        if(chunk > count - done) {
            chunk = count - done;
        }

        pframe_t* pframe = get_page(cache, index);

        // Written back, this file's own dirty pages can be reclaimed
        if(pframe == nullptr && cache->num_dirty > 0) {
            kick_writeback();
            writeback_locked(node, bmap, file_size);
            pframe = get_page(cache, index);
        }

        if(pframe == nullptr) {
            retval = -ENOMEM;
            break;
        }

        // A page written whole needs nothing from the disk
        if(0 == (pframe->flags & PFRAME_UPTODATE) && chunk < PAGE_SIZE) {
            retval = fill_page(node, bmap, pframe, file_size);

            if(retval < 0) {
                remove_page(cache, pframe);
                break;
            }
        }

        if(!cache->memory) {
            retval = allocate_span(node, bmap, pos, chunk);

            if(retval < 0) {
                if(0 == (pframe->flags & PFRAME_UPTODATE)) {
                    remove_page(cache, pframe);
                }

                break;
            }
        }

        uint8_t* data = page_data(pframe);
        memcpy(data + in_page, (const uint8_t*)(buffer) + done, chunk);

        pframe->flags |= PFRAME_UPTODATE | PFRAME_ACCESSED;

        if(!cache->memory) {
            mark_dirty(node, pframe, sector_span(node, in_page, chunk));
        }

        done += chunk;

        if((off_t)(pos + chunk) > file_size) {
            file_size = pos + chunk;
        }
    }

    // The writer pays for going over the limit with its own pages;
    // it already holds what the filesystem needs for writeback
    if(!cache->memory && over_dirty_limit()) {
        kick_writeback();
        writeback_locked(node, bmap, file_size);
    }

    mutex_release(&cache->lock);

    if(done == 0 && retval < 0) {
        return retval;
    }

    return done;
}

int fcache_fault(vnode_t* restrict node, fcache_bmap_t bmap, off_t offset, off_t size, pmap_t* restrict pmap, virt_addr_t virt,
    unsigned vprot)
{
    kassert(0 == page_offset(offset));

    fcache_t* cache = &node->pages;

    if(!cache->memory && !sector_size_valid(node->fs->volume)) {
        return -EINVAL;
    }

    if(offset < 0 || offset >= size) {
        return -ENXIO;
    }

    uint64_t index = offset / PAGE_SIZE;

    bool write = vprot & VPROT_WRITE;

    if(write && node->fs->readonly) {
        return -EROFS;
    }

    mutex_acquire(&cache->lock);

    if(write && !cache->memory && over_dirty_limit()) {
        kick_writeback();
        writeback_locked(node, bmap, size);
    }

    pframe_t* pframe = get_page(cache, index);

    if(pframe == nullptr) {
        mutex_release(&cache->lock);
        return -ENOMEM;
    }

    if(0 == (pframe->flags & PFRAME_UPTODATE)) {
        int retval = fill_page(node, bmap, pframe, size);

        if(retval < 0) {
            remove_page(cache, pframe);
            mutex_release(&cache->lock);
            return retval;
        }
    }

    if(write && !cache->memory) {
        size_t valid = valid_bytes(index, size);
        int retval = allocate_span(node, bmap, index * PAGE_SIZE, valid);

        if(retval < 0) {
            mutex_release(&cache->lock);
            return retval;
        }

        mark_dirty(node, pframe, sector_span(node, 0, valid));
    }

    pframe->flags |= PFRAME_ACCESSED;

    // Mapped under the cache lock, so a truncation either finds the mapping or comes before it,
    // and writeback either sees the page dirty or takes write access away before writing it
    phys_addr_t phys = pframe_phys(pframe);
    int retval = rmap_patch(pmap, virt, phys, vprot);

    if(retval == -EAGAIN) {
        pframe_incref(pframe);
        retval = rmap_map(pmap, virt, phys, vprot);

        if(retval < 0) {
            bool last = pframe_decref(pframe);
            kassert(!last); // The cache still holds its own
        }
    }

    mutex_release(&cache->lock);

    return retval;
}

int fcache_writeback(vnode_t* restrict node, fcache_bmap_t bmap, off_t size)
{
    fcache_t* cache = &node->pages;

    mutex_acquire(&cache->lock);
    int retval = writeback_locked(node, bmap, size);
    mutex_release(&cache->lock);

    return retval;
}

void fcache_truncate(vnode_t* restrict node, off_t size)
{
    fcache_t* cache = &node->pages;
    uint64_t keep = ((uint64_t)(size) + PAGE_SIZE - 1) / PAGE_SIZE;

    mutex_acquire(&cache->lock);

    void* batch[GATHER_BATCH];
    size_t count;

    while((count = radix_gather(&cache->tree, keep, batch, nullptr, GATHER_BATCH)) > 0) {
        for(size_t i = 0; i < count; ++i) {
            remove_page(cache, batch[i]);
        }
    }

    // Pages hold zeroes past the end of file, so
    // a later extension does not bring back cut data
    size_t tail = size % PAGE_SIZE;

    if(tail) {
        pframe_t* pframe = radix_lookup(&cache->tree, size / PAGE_SIZE);

        if(pframe) {
            uint8_t* data = page_data(pframe);
            memset(data + tail, 0, PAGE_SIZE - tail);
        }
    }

    mutex_release(&cache->lock);
}

void fcache_forget(vnode_t* restrict node, uint64_t offset, uint64_t length)
{
    fcache_t* cache = &node->pages;
    uint64_t end = offset + length;

    mutex_acquire(&cache->lock);

    for(uint64_t pos = offset; pos < end;) {
        size_t in_page = pos % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - in_page;

        if(chunk > end - pos) {
            chunk = end - pos;
        }

        pframe_t* pframe = radix_lookup(&cache->tree, pos / PAGE_SIZE);

        if(pframe) {
            pframe->flags &= ~(sector_span(node, in_page, chunk) << PFRAME_DIRTY_SHIFT);

            if(0 == (pframe->flags >> PFRAME_DIRTY_SHIFT)) {
                mark_clean(cache, pframe);
            }
        }

        pos += chunk;
    }

    mutex_release(&cache->lock);
}

int fcache_sync(vfs_t* restrict fs)
{
    size_t pending = 0;

    spinlock_acquire(&dirty_lock);

    for(vnode_t* node = dirty_head; node; node = node->pages.dirty_next) {
        if(fs == nullptr || node->fs == fs) {
            pending += 1;
        }
    }

    spinlock_release(&dirty_lock);

    int retval = 0;

    // Vnodes queued again while this runs wait for the next
    // sync, so a steady writer can't keep the loop going
    while(pending > 0) {
        vnode_t* picked = nullptr;

        spinlock_acquire(&dirty_lock);

        for(vnode_t** prev = &dirty_head; *prev; prev = &(*prev)->pages.dirty_next) {
            vnode_t* node = *prev;

            if(fs == nullptr || node->fs == fs) {
                *prev = node->pages.dirty_next;
                node->pages.dirty_next = nullptr;
                node->pages.queued = false;
                picked = node;
                break;
            }
        }

        spinlock_release(&dirty_lock);

        if(picked == nullptr) {
            break;
        }

        int node_retval = vnode_fsync(picked);

        if(node_retval < 0 && retval == 0) {
            retval = node_retval;
        }

        vnode_decref(picked);
        pending -= 1;
    }

    return retval;
}

vnode_t* fcache_dev_create(vfs_t* restrict fs)
{
    vnode_t* device_node = vnode_alloc(VNODE_BLOCK_DEV, &dev_ops, nullptr);

    if(device_node) {
        device_node->fs = fs;
    }

    return device_node;
}

void fcache_dev_destroy(vnode_t* restrict device_node)
{
    vnode_fsync(device_node);

    bool queued = false;

    spinlock_acquire(&dirty_lock);

    for(vnode_t** prev = &dirty_head; *prev; prev = &(*prev)->pages.dirty_next) {
        if(*prev == device_node) {
            *prev = device_node->pages.dirty_next;
            device_node->pages.dirty_next = nullptr;
            device_node->pages.queued = false;
            queued = true;
            break;
        }
    }

    spinlock_release(&dirty_lock);

    if(queued) {
        vnode_decref(device_node);
    }

    vnode_decref(device_node);
}

int fcache_dev_read(vnode_t* restrict device_node, uint64_t offset, void* restrict buffer, size_t size)
{
    ssize_t done = fcache_read(device_node, &dev_bmap, buffer, size, offset, dev_size(device_node));

    if(done < 0) {
        return done;
    }

    return (size_t)(done) == size ? 0 : -EIO;
}

int fcache_dev_write(vnode_t* restrict device_node, uint64_t offset, const void* restrict buffer, size_t size)
{
    ssize_t done = fcache_write(device_node, &dev_bmap, buffer, size, offset, dev_size(device_node));

    if(done < 0) {
        return done;
    }

    return (size_t)(done) == size ? 0 : -EIO;
}

size_t fcache_reclaim(size_t count)
{
    thread_t* thread = this_thread();

    if(!interrupt_state() || thread->preempt_count > 0) {
        return 0;
    }

    size_t freed = 0;

    spinlock_acquire(&lru_lock);

    // Twice round, so that a page left with only its second chance is taken
    size_t budget = 2 * lru_length;

    while(freed < count && budget > 0 && lru_head) {
        pframe_t* pframe = lru_head;
        fcache_t* cache = pframe->owner;

        budget -= 1;

        lru_unlink(pframe);
        lru_append(pframe);

        if(mutex_held(&cache->lock) || !mutex_try_acquire(&cache->lock)) {
            continue;
        }

        if(pframe->flags & PFRAME_ACCESSED) {
            pframe->flags &= ~PFRAME_ACCESSED;
            mutex_release(&cache->lock);
            continue;
        }

        if((pframe->flags & PFRAME_DIRTY) || pframe_shared(pframe)) {
            mutex_release(&cache->lock);
            continue;
        }

        lru_unlink(pframe);

        spinlock_release(&lru_lock);

        pframe_t* removed = radix_remove(&cache->tree, pframe->index);
        kassert(removed == pframe);

        cache->num_pages -= 1;

        pframe->flags &= ~(PFRAME_CACHE | PFRAME_UPTODATE);
        pframe->owner = nullptr;
        pframe->index = 0;

        mutex_release(&cache->lock);

        pslab_free_phys(pframe_phys(pframe));
        freed += 1;

        spinlock_acquire(&lru_lock);
    }

    spinlock_release(&lru_lock);

    return freed;
}

static void init_fcache(void)
{
    event_init(&writeback_wakeup);

    thread_t* writeback = thread_create_kernel(&writeback_thread, nullptr);

    if(writeback == nullptr) {
        panic("fcache: out of memory");
    }

    sched_enqueue(writeback);
}

initcall_bsp_vfs(init_fcache, INIT_FIRST);
