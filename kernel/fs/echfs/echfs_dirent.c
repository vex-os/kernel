#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/string.h>

#include "echfs_internal.h"

static uint64_t dirent_offset(const echfs_vfs_context_t* restrict context, uint64_t index)
{
    return context->dir_start * context->block_size + index * sizeof(echfs_dirent_t);
}

int echfs_dirent_find(const echfs_vfs_context_t* restrict context, uint64_t parent_id, const char* restrict name,
    uint64_t* restrict out_index)
{
    echfs_dirent_t entry;

    for(uint64_t i = 0; i < context->dir_entries; ++i) {
        int retval = echfs_dirent_read(context, i, &entry);

        if(retval < 0) {
            return retval;
        }

        if(entry.directory_id == ECHFS_DIR_ID_END) {
            return -ENOENT;
        }

        if(entry.directory_id == ECHFS_DIR_ID_DELETED) {
            continue;
        }

        if(entry.directory_id != parent_id) {
            continue;
        }

        if(0 == strncmp(entry.name, name, sizeof(entry.name))) {
            *out_index = i;
            return 0;
        }
    }

    return -ENOENT;
}

int echfs_dirent_read(const echfs_vfs_context_t* restrict context, uint64_t index, echfs_dirent_t* restrict out)
{
    if(index >= context->dir_entries) {
        return -EINVAL;
    }

    uint64_t offset = dirent_offset(context, index);

    return fcache_dev_read(context->device_node, offset, out, sizeof(echfs_dirent_t));
}

int echfs_dirent_write(echfs_vfs_context_t* restrict context, uint64_t index, const echfs_dirent_t* restrict entry)
{
    if(index >= context->dir_entries) {
        return -EINVAL;
    }

    uint64_t offset = dirent_offset(context, index);

    return fcache_dev_write(context->device_node, offset, entry, sizeof(echfs_dirent_t));
}

int echfs_dirent_alloc(echfs_vfs_context_t* restrict context, uint64_t* restrict out_index)
{
    echfs_dirent_t entry;

    for(uint64_t i = 0; i < context->dir_entries; ++i) {
        int retval = echfs_dirent_read(context, i, &entry);

        if(retval < 0) {
            return retval;
        }

        if(entry.directory_id == ECHFS_DIR_ID_END || entry.directory_id == ECHFS_DIR_ID_DELETED) {
            *out_index = i;
            return 0;
        }
    }

    return -ENOSPC;
}
