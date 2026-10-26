#include <vex/block_interface.h>
#include <vex/cmdline.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/mount.h>
#include <vex/slab.h>
#include <vex/string.h>
#include <vex/vnode.h>

#include "echfs_internal.h"

#define ECHFS_RESERVED_BLOCKS UINT64_C(16)

static int echfs_probe(vfs_t* restrict fs, const block_interface_t* restrict volume)
{
    echfs_identity_t identity;

    int retval = block_read_sector_prefix(volume, 0, &identity, sizeof(identity));

    if(retval < 0) {
        return retval;
    }

    if(memcmp(identity.signature, "_ECH_FS_", 8)) {
        return -ENODEV;
    }

    return 0;
}

static int echfs_mount(vfs_t* restrict fs, const block_interface_t* restrict volume, const cmdline_t* restrict params)
{
    if(volume == nullptr) {
        return -EINVAL;
    }

    size_t sector_size = volume->sector_size;

    echfs_identity_t* identity = kmalloc(sizeof(echfs_identity_t));

    if(identity == nullptr) {
        return -ENOMEM;
    }

    int retval = block_read_sector_prefix(volume, 0, identity, sizeof(*identity));

    if(retval < 0) {
        kfree(identity);
        return retval;
    }

    if(memcmp(identity->signature, "_ECH_FS_", 8)) {
        kfree(identity);
        return -EINVAL;
    }

    uint64_t block_size = identity->block_size;
    uint64_t total_blocks = identity->total_blocks;
    uint64_t maindir_size = identity->maindir_size;

    kfree(identity);

    if(block_size == 0 || block_size % sector_size) {
        return -EINVAL;
    }

    uint64_t sectors_per_block = block_size / sector_size;

    uint64_t fat_start = ECHFS_RESERVED_BLOCKS;
    uint64_t fat_size = (total_blocks * sizeof(uint64_t) + block_size - 1) / block_size;
    uint64_t dir_start = fat_start + fat_size;
    uint64_t dir_size = maindir_size;
    uint64_t data_start = dir_start + dir_size;

    echfs_vfs_context_t* context = kmalloc(sizeof(echfs_vfs_context_t));

    if(context == nullptr) {
        return -ENOMEM;
    }

    memset(context, 0, sizeof(echfs_vfs_context_t));

    mutex_init(&context->lock);

    context->volume = volume;
    context->block_size = block_size;
    context->sectors_per_block = sectors_per_block;
    context->total_blocks = total_blocks;
    context->fat_start = fat_start;
    context->fat_size = fat_size;
    context->dir_start = dir_start;
    context->dir_size = dir_size;
    context->data_start = data_start;
    context->dir_entries = dir_size * block_size / sizeof(echfs_dirent_t);

    context->next_free_hint = data_start;
    context->device_node = fcache_dev_create(fs);

    if(context->device_node == nullptr) {
        kfree(context);
        return -ENOMEM;
    }

    echfs_vnode_context_t* root_ctx = slab_alloc(sizeof(echfs_vnode_context_t));

    if(root_ctx == nullptr) {
        fcache_dev_destroy(context->device_node);
        kfree(context);
        return -ENOMEM;
    }

    root_ctx->fs_ctx = context;
    root_ctx->dir_entry = UINT64_MAX;
    root_ctx->start_block = ECHFS_DIR_ID_ROOT;
    root_ctx->size = 0;
    root_ctx->cached_block = ECHFS_DIR_ID_ROOT;
    root_ctx->cached_block_idx = 0;

    vnode_t* root = vnode_alloc(VNODE_DIRECTORY, &echfs_vnode_ops, root_ctx);

    if(root == nullptr) {
        slab_free(root_ctx);
        fcache_dev_destroy(context->device_node);
        kfree(context);
        return -ENOMEM;
    }

    root->fs = fs;
    root->ino = ECHFS_ROOT_NODENUMBER;

    fs->root = root;
    fs->ctxdata = context;

    return 0;
}

static int statvfs_locked(vfs_t* restrict fs, statvfs_t* restrict out)
{
    echfs_vfs_context_t* context = fs->ctxdata;

    memset(out, 0, sizeof(statvfs_t));

    size_t free_blocks = 0;
    size_t data_blocks = context->total_blocks - context->data_start;

    for(uint64_t i = context->data_start; i < context->total_blocks; ++i) {
        if(echfs_block_entry(context, i) == ECHFS_TABLE_FREE) {
            free_blocks += 1;
        }
    }

    out->f_bsize = context->volume->sector_size;
    out->f_frsize = context->block_size;
    out->f_blocks = data_blocks;
    out->f_bfree = free_blocks;
    out->f_bavail = free_blocks;

    out->f_flag = ST_NOSUID;
    out->f_namemax = 200;

    return 0;
}

static int fill_node_locked(vfs_t* restrict fs, vnode_t* restrict node)
{
    echfs_vfs_context_t* context = fs->ctxdata;

    uint64_t index = (uint64_t)(node->ino);

    if(index >= context->dir_entries) {
        return -ENOENT;
    }

    echfs_dirent_t entry;
    int retval = echfs_dirent_read(context, index, &entry);

    if(retval < 0) {
        return retval;
    }

    if(entry.directory_id == ECHFS_DIR_ID_END || entry.directory_id == ECHFS_DIR_ID_DELETED) {
        return -ENOENT;
    }

    echfs_vnode_context_t* ctx = slab_alloc(sizeof(echfs_vnode_context_t));

    if(ctx == nullptr) {
        return -ENOMEM;
    }

    ctx->fs_ctx = context;
    ctx->dir_entry = index;
    ctx->start_block = entry.start_block;
    ctx->size = entry.file_size;
    ctx->cached_block = entry.start_block;
    ctx->cached_block_idx = 0;

    node->ctxdata = ctx;

    if(entry.object_type == 1) {
        node->type = VNODE_DIRECTORY;
    }
    else {
        node->type = VNODE_FILE;
    }

    return 0;
}

static int echfs_unmount(vfs_t* restrict fs)
{
    echfs_vfs_context_t* context = fs->ctxdata;

    vnode_decref(fs->root);

    fcache_dev_destroy(context->device_node);
    mutex_deinit(&context->lock);
    kfree(context);

    return 0;
}

static int echfs_statvfs(vfs_t* restrict fs, statvfs_t* restrict out)
{
    echfs_vfs_context_t* context = fs->ctxdata;
    mutex_acquire(&context->lock);
    int retval = statvfs_locked(fs, out);
    mutex_release(&context->lock);
    return retval;
}

static int echfs_fill_node(vfs_t* restrict fs, vnode_t* restrict node)
{
    echfs_vfs_context_t* context = fs->ctxdata;
    mutex_acquire(&context->lock);
    int retval = fill_node_locked(fs, node);
    mutex_release(&context->lock);
    return retval;
}

static int echfs_sync(vfs_t* restrict fs)
{
    echfs_vfs_context_t* context = fs->ctxdata;
    return vnode_fsync(context->device_node);
}

static const vfs_ops_t echfs_vfs_ops = {
    .probe = &echfs_probe,
    .mount = &echfs_mount,
    .statvfs = &echfs_statvfs,
    .fill_node = &echfs_fill_node,
    .unmount = &echfs_unmount,
    .sync = &echfs_sync,
};

static vfs_driver_t echfs_vfs_driver = {
    .name = "echfs",
    .ops = &echfs_vfs_ops,
};

static void init_echfs(void)
{
    vfs_driver_add(&echfs_vfs_driver);
}

initcall_bsp_fs(init_echfs, INIT_EARLY);
