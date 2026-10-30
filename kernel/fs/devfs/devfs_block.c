#include <vex/block_event.h>
#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/kmalloc.h>
#include <vex/limits.h>
#include <vex/mutex.h>
#include <vex/slab.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/usage.h>
#include <vex/vnode.h>

#include "devfs_internal.h"

typedef struct devfs_block_context_s devfs_block_context_t;

struct devfs_block_context_s {
    const block_interface_t* interface;
    mutex_t bounce_lock; // Held across the I/O that fills bounce_buffer
    uint8_t* bounce_buffer;
};

static_assert(sizeof(devfs_block_context_t) <= SLAB_MAX_SIZE);

static void devfs_block_release(vnode_t* restrict node)
{
    devfs_block_context_t* context = node->ctxdata;
    block_interface_decref(context->interface);
    mutex_deinit(&context->bounce_lock);
    kfree(context->bounce_buffer);
    slab_free(context);
}

static ssize_t devfs_block_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    devfs_block_context_t* context = node->ctxdata;
    const block_interface_t* interface = context->interface;

    uint8_t* buffer_ptr = buffer;

    size_t sector_size = interface->sector_size;
    size_t byte_offset = offset % sector_size;
    uintptr_t lba = offset / sector_size;

    size_t remaining = size;
    uintptr_t current = lba;

    if(size == 0) {
        return 0;
    }

    if(byte_offset == 0 && (size % sector_size) == 0) {
        int retval = block_read(interface, lba, size / sector_size, buffer);

        if(retval < 0)
            return retval;
        return size;
    }

    if(byte_offset) {
        size_t chunk = sector_size - byte_offset;

        if(chunk > remaining) {
            chunk = remaining;
        }

        mutex_acquire(&context->bounce_lock);
        int retval = block_read(interface, current, 1, context->bounce_buffer);

        if(retval < 0) {
            mutex_release(&context->bounce_lock);
            return retval;
        }

        memcpy(buffer_ptr, context->bounce_buffer + byte_offset, chunk);
        mutex_release(&context->bounce_lock);

        buffer_ptr += chunk;
        remaining -= chunk;
        current += 1;
    }

    if(remaining >= sector_size) {
        size_t full = remaining / sector_size;
        int retval = block_read(interface, current, full, buffer_ptr);

        if(retval < 0) {
            return retval;
        }

        buffer_ptr += full * sector_size;
        remaining -= full * sector_size;
        current += full;
    }

    if(remaining > 0) {
        mutex_acquire(&context->bounce_lock);

        int retval = block_read(interface, current, 1, context->bounce_buffer);

        if(retval < 0) {
            mutex_release(&context->bounce_lock);
            return retval;
        }

        memcpy(buffer_ptr, context->bounce_buffer, remaining);

        mutex_release(&context->bounce_lock);
    }

    return size;
}

static ssize_t write_bytes(vnode_t* restrict node, const void* restrict buffer, size_t size, off_t offset)
{
    devfs_block_context_t* context = node->ctxdata;
    const block_interface_t* interface = context->interface;

    const uint8_t* buffer_ptr = buffer;

    size_t sector_size = interface->sector_size;
    size_t byte_offset = offset % sector_size;
    uintptr_t lba = offset / sector_size;

    size_t remaining = size;
    uintptr_t current = lba;

    if(size == 0) {
        return 0;
    }

    if(byte_offset == 0 && (size % sector_size) == 0) {
        int retval = block_write(interface, lba, size / sector_size, buffer);

        if(retval < 0)
            return retval;
        return size;
    }

    if(byte_offset) {
        size_t chunk = sector_size - byte_offset;

        if(chunk > remaining) {
            chunk = remaining;
        }

        mutex_acquire(&context->bounce_lock);

        int retval = block_read(interface, current, 1, context->bounce_buffer);

        if(retval < 0) {
            mutex_release(&context->bounce_lock);
            return retval;
        }

        memcpy(context->bounce_buffer + byte_offset, buffer_ptr, chunk);

        retval = block_write(interface, current, 1, context->bounce_buffer);

        mutex_release(&context->bounce_lock);

        if(retval < 0) {
            return retval;
        }

        buffer_ptr += chunk;
        remaining -= chunk;
        current += 1;
    }

    if(remaining >= sector_size) {
        size_t full = remaining / sector_size;
        int retval = block_write(interface, current, full, buffer_ptr);

        if(retval < 0) {
            return retval;
        }

        buffer_ptr += full * sector_size;
        remaining -= full * sector_size;
        current += full;
    }

    if(remaining > 0) {
        mutex_acquire(&context->bounce_lock);

        int retval = block_read(interface, current, 1, context->bounce_buffer);

        if(retval < 0) {
            mutex_release(&context->bounce_lock);
            return retval;
        }

        memcpy(context->bounce_buffer, buffer_ptr, remaining);

        retval = block_write(interface, current, 1, context->bounce_buffer);

        mutex_release(&context->bounce_lock);

        if(retval < 0) {
            return retval;
        }
    }

    return size;
}

// The write goes to the device at once, with no page cache to count it when it gets dirty
static ssize_t devfs_block_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    ssize_t written = write_bytes(node, buffer, size, offset);

    if(written > 0) {
        thread_t* thread = this_thread();
        usage_increase(&thread->usage.blocks_out, usage_blocks_of(written));
    }

    return written;
}

static int devfs_block_stat(vnode_t* restrict node, stat_t* restrict out)
{
    const devfs_block_context_t* context = node->ctxdata;
    const block_interface_t* interface = context->interface;

    memset(out, 0, sizeof(stat_t));

    out->st_mode = S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    out->st_rdev = interface->priv.dev;

    out->st_size = interface->sector_count * interface->sector_size;
    out->st_blksize = interface->sector_size;
    out->st_blocks = (out->st_size + S_BLKSIZE - 1) / S_BLKSIZE;

    return 0;
}

static const vnode_ops_t devfs_block_ops = {
    .release = &devfs_block_release,
    .read = &devfs_block_read,
    .write = &devfs_block_write,
    .stat = &devfs_block_stat,
};

void devfs_block_event_handler(unsigned event, const block_interface_t* restrict interface, void* restrict ctxdata)
{
    if(interface->flags & BLOCK_FLAG_HIDDEN) {
        return;
    }

    if(event == BLOCK_EVENT_MEDIA_CONNECTED || event == BLOCK_EVENT_PARTITION_ADDED) {
        devfs_block_context_t* context = slab_alloc(sizeof(devfs_block_context_t));

        if(context == nullptr) {
            return;
        }

        context->interface = interface;
        mutex_init(&context->bounce_lock);
        context->bounce_buffer = kmalloc(interface->sector_size);

        if(context->bounce_buffer == nullptr) {
            slab_free(context);
            return;
        }

        vnode_t* vnode = vnode_alloc(VNODE_BLOCK_DEV, &devfs_block_ops, context);

        if(vnode == nullptr) {
            kfree(context->bounce_buffer);
            slab_free(context);
            return;
        }

        vnode->volume = interface;
        block_interface_incref(interface); // Dropped by devfs_block_release

        int retval = devfs_insert(ctxdata, interface->name, vnode);

        if(retval < 0) {
            vnode_decref(vnode);
        }

        return;
    }

    if(event == BLOCK_EVENT_MEDIA_DISCONNECTED || event == BLOCK_EVENT_PARTITION_REMOVED) {
        devfs_remove(ctxdata, interface->name);
        return;
    }
}
