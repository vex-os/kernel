#include <vex/char_event.h>
#include <vex/char_interface.h>
#include <vex/errno.h>
#include <vex/file.h>
#include <vex/slab.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/vms.h>
#include <vex/vnode.h>

#include "devfs_internal.h"

typedef struct devfs_char_context_s devfs_char_context_t;

struct devfs_char_context_s {
    const char_interface_t* interface;
};

static_assert(sizeof(devfs_char_context_t) <= SLAB_MAX_SIZE);

static void devfs_char_release(vnode_t* restrict node)
{
    devfs_char_context_t* context = node->ctxdata;
    char_interface_decref(context->interface);
    slab_free(context);
}

static int devfs_char_open(vnode_t* restrict node, file_t* restrict file)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_open(context->interface, file->flags, &file->ctxdata);
}

static int devfs_char_close(vnode_t* restrict node, file_t* restrict file)
{
    const devfs_char_context_t* context = node->ctxdata;

    char_interface_close(context->interface, file->ctxdata);

    return 0;
}

static ssize_t devfs_char_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_read(context->interface, file->ctxdata, offset, buffer, size, file_flags(file));
}

static ssize_t devfs_char_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_write(context->interface, file->ctxdata, offset, buffer, size, file_flags(file));
}

static off_t devfs_char_seek(vnode_t* restrict node, file_t* restrict file, off_t offset, int origin)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_seek(context->interface, file->ctxdata, file->offset, offset, origin);
}

static int devfs_char_ioctl(vnode_t* restrict node, file_t* restrict file, unsigned request, uintptr_t argument)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_ioctl(context->interface, file->ctxdata, request, argument);
}

static unsigned devfs_char_poll(vnode_t* restrict node, file_t* restrict file, poll_table_t* restrict table)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_poll(context->interface, file->ctxdata, table);
}

static int devfs_char_mmap(vnode_t* restrict node, off_t offset, size_t length)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_mmap(context->interface, offset, length);
}

static int devfs_char_resolve_page(vnode_t* restrict node, off_t offset, phys_addr_t* restrict phys, unsigned* restrict cache)
{
    const devfs_char_context_t* context = node->ctxdata;

    return char_interface_resolve_page(context->interface, offset, phys, cache);
}

static int devfs_char_stat(vnode_t* restrict node, stat_t* restrict out)
{
    const devfs_char_context_t* context = node->ctxdata;
    const char_interface_t* interface = context->interface;

    memset(out, 0, sizeof(stat_t));

    out->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    out->st_rdev = interface->priv.dev;

    return 0;
}

static const vnode_ops_t devfs_char_ops = {
    .release = &devfs_char_release,
    .read = &devfs_char_read,
    .write = &devfs_char_write,
    .seek = &devfs_char_seek,
    .ioctl = &devfs_char_ioctl,
    .poll = &devfs_char_poll,
    .stat = &devfs_char_stat,
    .open = &devfs_char_open,
    .close = &devfs_char_close,
    .mmap = &devfs_char_mmap,
    .resolve_page = &devfs_char_resolve_page,
};

static void revoke_mappings(vfs_t* restrict fs, const char_interface_t* restrict interface)
{
    vnode_t* node = devfs_find(fs, interface->name);

    if(node) {
        vms_revoke(node);
        vnode_decref(node);
    }
}

void devfs_char_event_handler(unsigned event, const char_interface_t* restrict interface, void* restrict ctxdata)
{
    if(event == CHAR_EVENT_INTERFACE_ADDED) {
        devfs_char_context_t* context = slab_alloc(sizeof(devfs_char_context_t));

        if(context == nullptr) {
            return;
        }

        context->interface = interface;

        vnode_t* vnode = vnode_alloc(VNODE_CHAR_DEV, &devfs_char_ops, context);

        if(vnode == nullptr) {
            slab_free(context);
            return;
        }

        char_interface_incref(interface); // Dropped by devfs_char_release

        int retval = devfs_insert(ctxdata, interface->name, vnode);

        if(retval < 0) {
            vnode_decref(vnode);
        }

        return;
    }

    if(event == CHAR_EVENT_INTERFACE_REMOVED) {
        revoke_mappings(ctxdata, interface);
        devfs_remove(ctxdata, interface->name);
        return;
    }

    if(event == CHAR_EVENT_MEMORY_CHANGED) {
        revoke_mappings(ctxdata, interface);
        return;
    }
}
