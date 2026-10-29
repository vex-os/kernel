#include <vex/assert.h>
#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/hart.h>
#include <vex/hhdm.h>
#include <vex/page.h>
#include <vex/pdma.h>
#include <vex/pmap.h>
#include <vex/string.h>

#define BLOCK_MAX_SEGMENTS 32 // Most segments block_read and block_write put in one request

static void finish(block_request_t* restrict req, int status)
{
    req->status = status;
    req->done(req);
}

static bool request_valid(const block_interface_t* restrict interface, const block_request_t* restrict req)
{
    if(req->op == BLOCK_REQ_FLUSH) {
        return req->length == 0 && req->num_segments == 0;
    }

    if(req->op != BLOCK_REQ_READ && req->op != BLOCK_REQ_WRITE) {
        return false;
    }

    if(req->length == 0 || req->length % interface->sector_size || req->length > interface->max_transfer) {
        return false;
    }

    if(req->num_segments == 0 || req->num_segments > interface->max_segments) {
        return false;
    }

    size_t count = req->length / interface->sector_size;

    if(req->lba >= interface->sector_count || count > interface->sector_count - req->lba) {
        return false;
    }

    size_t total = 0;

    for(size_t i = 0; i < req->num_segments; ++i) {
        const block_segment_t* segment = &req->segments[i];

        if(segment->length == 0 || ((segment->phys.value | segment->length) & interface->dma_align)) {
            return false;
        }

        if(address_less(interface->dma_limit, segment->phys) || segment->length - 1 > address_diff(interface->dma_limit, segment->phys)) {
            return false;
        }

        total += segment->length;
    }

    return total == req->length;
}

static void wake_waiter(block_request_t* restrict req)
{
    event_trigger(req->ctxdata, true);
}

static bool map_segments(const block_interface_t* restrict interface, const uint8_t* restrict buffer, size_t length,
    block_segment_t* restrict segments, size_t* restrict num_segments)
{
    size_t count = 0;

    while(length > 0) {
        uintptr_t virt = (uintptr_t)(buffer);
        phys_addr_t phys = pmap_resolve_address(&sys_pmap, virt);
        size_t piece = PAGE_SIZE - (virt % PAGE_SIZE);

        if(piece > length) {
            piece = length;
        }

        if(address_null(phys) || ((phys.value | piece) & interface->dma_align) || address_less(interface->dma_limit, phys)
            || piece - 1 > address_diff(interface->dma_limit, phys)) {
            return false;
        }

        if(count && address_equal(address_add(segments[count - 1].phys, segments[count - 1].length), phys)) {
            segments[count - 1].length += piece;
        }
        else {
            if(count >= interface->max_segments || count >= BLOCK_MAX_SEGMENTS) {
                return false;
            }

            segments[count].phys = phys;
            segments[count].length = piece;
            count += 1;
        }

        buffer += piece;
        length -= piece;
    }

    *num_segments = count;

    return true;
}

static int transfer(const block_interface_t* restrict interface, unsigned op, uintptr_t lba, size_t count, uint8_t* restrict buffer)
{
    if(0 == (interface->flags & BLOCK_FLAG_PRESENT)) {
        return -ENODEV;
    }

    size_t sector_size = interface->sector_size;

    if(lba >= interface->sector_count || count > interface->sector_count - lba) {
        return -EINVAL;
    }

    size_t max_segments = BLOCK_MAX_SEGMENTS;

    if(interface->max_segments < BLOCK_MAX_SEGMENTS) {
        max_segments = interface->max_segments;
    }

    if(max_segments == 0) {
        return -EIO;
    }

    size_t max_bytes = (max_segments - 1) * PAGE_SIZE;

    if(max_segments == 1 || max_bytes > interface->max_transfer) {
        max_bytes = interface->max_transfer;
    }

    size_t max_sectors = max_bytes / sector_size;

    if(max_sectors == 0) {
        return -EIO;
    }

    phys_addr_t bounce_phys = phys_addr(0);
    size_t bounce_pages = 0;
    int retval = 0;

    while(count > 0 && retval == 0) {
        size_t chunk = count < max_sectors ? count : max_sectors;
        size_t bytes = chunk * sector_size;

        block_segment_t segments[BLOCK_MAX_SEGMENTS];
        block_request_t req = {};
        req.op = op;
        req.lba = lba;
        req.length = bytes;
        req.segments = segments;

        bool direct = map_segments(interface, buffer, bytes, segments, &req.num_segments);

        if(!direct) {
            if(address_null(bounce_phys)) {
                bounce_pages = (max_sectors * sector_size + PAGE_SIZE - 1) / PAGE_SIZE;
                bounce_phys = pdma_alloc_phys(bounce_pages);

                if(address_null(bounce_phys)) {
                    return -ENOMEM;
                }
            }

            segments[0].phys = bounce_phys;
            segments[0].length = bytes;
            req.num_segments = 1;

            if(op == BLOCK_REQ_WRITE) {
                memcpy(phys_to_virt(bounce_phys), buffer, bytes);
            }
        }

        retval = block_submit_wait(interface, &req);

        if(retval == 0 && !direct && op == BLOCK_REQ_READ) {
            memcpy(buffer, phys_to_virt(bounce_phys), bytes);
        }

        buffer += bytes;
        lba += chunk;
        count -= chunk;
    }

    if(address_valid(bounce_phys)) {
        pdma_free_phys(bounce_phys, bounce_pages);
    }

    return retval;
}

void block_submit(const block_interface_t* restrict interface, block_request_t* restrict req)
{
    if(0 == (interface->flags & BLOCK_FLAG_PRESENT)) {
        finish(req, -ENODEV);
        return;
    }

    if(!request_valid(interface, req)) {
        finish(req, -EINVAL);
        return;
    }

    interface->ops->submit(interface, req);
}

int block_submit_wait(const block_interface_t* restrict interface, block_request_t* restrict req)
{
    event_t done;
    event_t* event = &done;

    event_init(&done);

    req->done = &wake_waiter;
    req->ctxdata = &done;

    block_submit(interface, req);

    if(event_await(&event, 1) < 0) {
        kassert(interrupt_state());

        while(event_await_blocking(&event, 1) < 0) {
            // empty
        }
    }

    event_deinit(&done);

    return req->status;
}

int block_read(const block_interface_t* restrict interface, uintptr_t lba, size_t count, void* restrict buffer)
{
    return transfer(interface, BLOCK_REQ_READ, lba, count, buffer);
}

int block_write(const block_interface_t* restrict interface, uintptr_t lba, size_t count, const void* restrict buffer)
{
    return transfer(interface, BLOCK_REQ_WRITE, lba, count, (uint8_t*)(buffer));
}

int block_flush(const block_interface_t* restrict interface)
{
    block_request_t req = {};
    req.op = BLOCK_REQ_FLUSH;
    return block_submit_wait(interface, &req);
}
