#include <vex/block_interface.h>
#include <vex/cmdline.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/mount.h>
#include <vex/slab.h>
#include <vex/string.h>
#include <vex/vnode.h>
#include <vex/wallclock.h>

#include "ext2_internal.h"

static_assert(sizeof(ext2_vnode_context_t) <= SLAB_MAX_SIZE);

static int read_super(const block_interface_t* restrict volume, ext2_superblock_t* restrict out)
{
    size_t sector_size = volume->sector_size;
    uintptr_t lba = EXT2_SUPER_OFFSET / sector_size;
    size_t skip = EXT2_SUPER_OFFSET % sector_size;
    size_t count = (skip + sizeof(ext2_superblock_t) + sector_size - 1) / sector_size;

    uint8_t* buffer = kmalloc(count * sector_size);

    if(buffer == nullptr) {
        return -ENOMEM;
    }

    int retval = block_read(volume, lba, count, buffer);

    if(retval == 0) {
        memcpy(out, buffer + skip, sizeof(ext2_superblock_t));
    }

    kfree(buffer);

    return retval;
}

static int setup_geometry(ext2_vfs_context_t* restrict context)
{
    const ext2_superblock_t* super = &context->super;
    const char* name = context->volume->name;
    size_t sector_size = context->volume->sector_size;

    if(le16toh(super->s_magic) != EXT2_SUPER_MAGIC) {
        return -EINVAL;
    }

    uint32_t rev_level = le32toh(super->s_rev_level);

    if(rev_level > EXT2_DYNAMIC_REV) {
        pr_warning("ext2: %s: unknown revision %u", name, rev_level);
        return -EINVAL;
    }

    uint32_t log_block_size = le32toh(super->s_log_block_size);

    if(log_block_size > EXT2_MAX_BLOCK_LOG_SIZE - EXT2_MIN_BLOCK_LOG_SIZE) {
        return -EINVAL;
    }

    size_t block_size = (size_t)(1) << (EXT2_MIN_BLOCK_LOG_SIZE + log_block_size);

    if(block_size % sector_size) {
        pr_warning("ext2: %s: block size %zu is not a multiple of the sector size %zu", name, block_size, sector_size);
        return -EINVAL;
    }

    uint32_t blocks_count = le32toh(super->s_blocks_count);
    uint32_t inodes_count = le32toh(super->s_inodes_count);
    uint32_t first_data_block = le32toh(super->s_first_data_block);
    uint32_t blocks_per_group = le32toh(super->s_blocks_per_group);
    uint32_t inodes_per_group = le32toh(super->s_inodes_per_group);
    size_t bits_per_block = block_size * 8;

    bool bad_blocks = blocks_per_group == 0 || blocks_per_group > bits_per_block;
    bool bad_inodes = inodes_per_group == 0 || inodes_per_group > bits_per_block;

    if(bad_blocks || bad_inodes) {
        return -EINVAL;
    }

    // The superblock is always 1024 bytes in, so it's in block 1 with 1 KiB blocks and in block 0 otherwise
    if(first_data_block != EXT2_SUPER_OFFSET / block_size || first_data_block >= blocks_count) {
        return -EINVAL;
    }

    size_t sectors_per_block = block_size / sector_size;

    if(blocks_count > context->volume->sector_count / sectors_per_block) {
        pr_warning("ext2: %s: filesystem is larger than the device", name);
        return -EINVAL;
    }

    uint32_t groups_count = (blocks_count - first_data_block + blocks_per_group - 1) / blocks_per_group;

    if((uint64_t)(groups_count) * (inodes_per_group) != inodes_count) { // clang-format antics
        return -EINVAL;
    }

    size_t inode_size = EXT2_GOOD_OLD_INODE_SIZE;
    uint32_t first_ino = EXT2_GOOD_OLD_FIRST_INO;

    if(rev_level == EXT2_DYNAMIC_REV) {
        inode_size = le16toh(super->s_inode_size);
        first_ino = le32toh(super->s_first_ino);

        if(inode_size < EXT2_GOOD_OLD_INODE_SIZE || inode_size > block_size || (inode_size & (inode_size - 1))) {
            return -EINVAL;
        }

        if(first_ino <= EXT2_ROOT_INO || first_ino > inodes_count) {
            return -EINVAL;
        }

        uint32_t incompat = le32toh(super->s_feature_incompat);

        if(incompat & ~EXT2_FEATURE_INCOMPAT_SUPP) {
            pr_warning("ext2: %s: unsupported incompatible features %#X", name, incompat & ~EXT2_FEATURE_INCOMPAT_SUPP);
            return -EINVAL;
        }
    }

    context->creator_os = le32toh(super->s_creator_os);
    context->block_size = block_size;
    context->sectors_per_block = sectors_per_block;
    context->addr_per_block = block_size / sizeof(uint32_t);
    context->inode_size = inode_size;
    context->groups_offset = ext2_block_offset(context, first_data_block + 1);
    context->groups_count = groups_count;
    context->blocks_per_group = blocks_per_group;
    context->inodes_per_group = inodes_per_group;
    context->first_data_block = first_data_block;
    context->first_ino = first_ino;

    return 0;
}

static bool must_mount_readonly(const ext2_vfs_context_t* restrict context)
{
    const ext2_superblock_t* super = &context->super;
    const char* name = context->volume->name;

    if(context->creator_os > EXT2_OS_LITES) {
        pr_notice("ext2: %s: unknown creator OS %u, mounting read-only", name, context->creator_os);
        return true;
    }

    if(le32toh(super->s_rev_level) == EXT2_GOOD_OLD_REV) {
        return false;
    }

    uint32_t ro_compat = le32toh(super->s_feature_ro_compat);

    if(ro_compat & ~EXT2_FEATURE_RO_COMPAT_SUPP) {
        pr_notice("ext2: %s: unsupported read-only features %#X, mounting read-only", name, ro_compat & ~EXT2_FEATURE_RO_COMPAT_SUPP);
        return true;
    }

    return false;
}

static bool group_block_valid(const ext2_vfs_context_t* restrict context, uint32_t block, uint32_t count)
{
    uint32_t blocks_count = le32toh(context->super.s_blocks_count);
    return block >= context->first_data_block && block < blocks_count && count <= blocks_count - block;
}

static int load_groups(ext2_vfs_context_t* restrict context)
{
    size_t size = context->groups_count * sizeof(ext2_group_desc_t);

    context->groups = kmalloc(size);

    if(context->groups == nullptr) {
        return -ENOMEM;
    }

    int retval = fcache_dev_read(context->device_node, context->groups_offset, context->groups, size);

    if(retval < 0) {
        return retval;
    }

    size_t table_size = context->inodes_per_group * context->inode_size;
    uint32_t table_blocks = (table_size + context->block_size - 1) / context->block_size;

    for(uint32_t i = 0; i < context->groups_count; ++i) {
        const ext2_group_desc_t* desc = &context->groups[i];

        uint32_t block_bitmap = le32toh(desc->bg_block_bitmap);
        uint32_t inode_bitmap = le32toh(desc->bg_inode_bitmap);
        uint32_t inode_table = le32toh(desc->bg_inode_table);

        bool valid = true;
        valid = valid && group_block_valid(context, block_bitmap, 1);
        valid = valid && group_block_valid(context, inode_bitmap, 1);
        valid = valid && group_block_valid(context, inode_table, table_blocks);

        if(!valid) {
            pr_warning("ext2: %s: group %u points outside the filesystem", context->volume->name, i);
            return -EINVAL;
        }
    }

    return 0;
}

static void free_context(ext2_vfs_context_t* restrict context)
{
    if(context->device_node) {
        fcache_dev_destroy(context->device_node);
    }

    if(context->groups) {
        kfree(context->groups);
    }

    mutex_deinit(&context->lock);
    kfree(context);
}

static int fill_root(vfs_t* restrict fs, ext2_vfs_context_t* restrict context)
{
    ext2_vnode_context_t* root_context = slab_alloc(sizeof(ext2_vnode_context_t));

    if(root_context == nullptr) {
        return -ENOMEM;
    }

    root_context->fs_ctx = context;

    int retval = ext2_inode_read(context, EXT2_ROOT_INO, &root_context->inode);

    if(retval < 0) {
        slab_free(root_context);
        return retval;
    }

    if(EXT2_S_IFDIR != (le16toh(root_context->inode.i_mode) & S_IFMT)) {
        pr_warning("ext2: %s: root inode is not a directory", context->volume->name);
        slab_free(root_context);
        return -EINVAL;
    }

    vnode_t* root = vnode_alloc(VNODE_DIRECTORY, &ext2_vnode_ops, root_context);

    if(root == nullptr) {
        slab_free(root_context);
        return -ENOMEM;
    }

    root->fs = fs;
    root->ino = EXT2_ROOT_INO;

    fs->root = root;

    return 0;
}

static int mark_mounted(ext2_vfs_context_t* restrict context)
{
    ext2_superblock_t* super = &context->super;
    int16_t max_mnt_count = (int16_t)(le16toh(super->s_max_mnt_count));
    uint16_t mnt_count = le16toh(super->s_mnt_count) + 1;

    if(max_mnt_count > 0 && mnt_count >= max_mnt_count) {
        pr_notice("ext2: %s: maximal mount count reached, running e2fsck is recommended", context->volume->name);
    }

    context->mount_state = le16toh(super->s_state);

    super->s_mnt_count = htole16(mnt_count);
    super->s_mtime = htole32((uint32_t)(wallclock_now()));
    super->s_state = htole16(context->mount_state & ~EXT2_VALID_FS);

    return ext2_super_commit(context);
}

static int ext2_probe(vfs_t* restrict fs, const block_interface_t* restrict volume)
{
    ext2_superblock_t* super = kmalloc(sizeof(ext2_superblock_t));

    if(super == nullptr) {
        return -ENOMEM;
    }

    int retval = read_super(volume, super);

    if(retval == 0 && le16toh(super->s_magic) != EXT2_SUPER_MAGIC) {
        retval = -ENODEV;
    }

    kfree(super);

    return retval;
}

static int ext2_mount(vfs_t* restrict fs, const block_interface_t* restrict volume, const cmdline_t* restrict params)
{
    if(volume == nullptr) {
        return -EINVAL;
    }

    ext2_vfs_context_t* context = kmalloc(sizeof(ext2_vfs_context_t));

    if(context == nullptr) {
        return -ENOMEM;
    }

    memset(context, 0, sizeof(ext2_vfs_context_t));

    mutex_init(&context->lock);

    context->volume = volume;

    int retval = read_super(volume, &context->super);

    if(retval == 0) {
        retval = setup_geometry(context);
    }

    if(retval < 0) {
        free_context(context);
        return retval;
    }

    uint16_t state = le16toh(context->super.s_state);

    if(0 == (state & EXT2_VALID_FS) || (state & EXT2_ERROR_FS)) {
        pr_warning("ext2: %s: filesystem was not cleanly unmounted or has errors, running e2fsck is recommended", volume->name);
    }

    context->device_node = fcache_dev_create(fs);

    if(context->device_node == nullptr) {
        free_context(context);
        return -ENOMEM;
    }

    retval = load_groups(context);

    if(retval == 0) {
        retval = fill_root(fs, context);
    }

    if(retval < 0) {
        free_context(context);
        return retval;
    }

    fs->ctxdata = context;
    if(must_mount_readonly(context)) {
        fs->readonly = true;
    }

    if(!fs->readonly) {
        retval = mark_mounted(context);

        if(retval < 0) {
            vnode_decref(fs->root);
            free_context(context);
            return retval;
        }
    }

    return 0;
}

static int ext2_unmount(vfs_t* restrict fs)
{
    ext2_vfs_context_t* context = fs->ctxdata;

    vnode_decref(fs->root);

    if(!fs->readonly) {
        context->super.s_state = htole16(context->mount_state);

        if(ext2_super_commit(context) < 0) {
            pr_warning("ext2: %s: failed to mark the filesystem clean", context->volume->name);
        }
    }

    free_context(context);

    return 0;
}

static int ext2_statvfs(vfs_t* restrict fs, statvfs_t* restrict out)
{
    ext2_vfs_context_t* context = fs->ctxdata;
    const ext2_superblock_t* super = &context->super;

    memset(out, 0, sizeof(statvfs_t));

    mutex_acquire(&context->lock);

    uint32_t free_blocks = le32toh(super->s_free_blocks_count);
    uint32_t reserved_blocks = le32toh(super->s_r_blocks_count);

    out->f_bsize = context->block_size;
    out->f_frsize = context->block_size;
    out->f_blocks = le32toh(super->s_blocks_count);
    out->f_bfree = free_blocks;
    out->f_files = le32toh(super->s_inodes_count);
    out->f_ffree = le32toh(super->s_free_inodes_count);
    out->f_namemax = EXT2_NAME_LEN;

    mutex_release(&context->lock);

    if(fs->readonly) {
        out->f_flag |= ST_RDONLY;
    }
    else {
        if(free_blocks > reserved_blocks) {
            out->f_bavail = free_blocks - reserved_blocks;
        }

        out->f_favail = out->f_ffree;
    }

    return 0;
}

static int ext2_fill_node(vfs_t* restrict fs, vnode_t* restrict node)
{
    ext2_vfs_context_t* context = fs->ctxdata;

    if(node->ino > UINT32_MAX) {
        return -ENOENT;
    }

    ext2_vnode_context_t* node_context = slab_alloc(sizeof(ext2_vnode_context_t));

    if(node_context == nullptr) {
        return -ENOMEM;
    }

    node_context->fs_ctx = context;

    mutex_acquire(&context->lock);
    int retval = ext2_inode_read(context, (uint32_t)(node->ino), &node_context->inode);
    mutex_release(&context->lock);

    if(retval == 0 && 0 == le16toh(node_context->inode.i_links_count)) {
        retval = -ENOENT;
    }

    if(retval == 0) {
        uint16_t mode = le16toh(node_context->inode.i_mode);

        retval = ext2_inode_nodetype(mode, &node->type);
    }

    if(retval == 0 && node->type == VNODE_FIFO) {
        vnode_init_special(node, 0);
    }

    if(retval < 0) {
        slab_free(node_context);
        return retval;
    }

    node->ctxdata = node_context;

    return 0;
}

static int ext2_sync(vfs_t* restrict fs)
{
    ext2_vfs_context_t* context = fs->ctxdata;
    return vnode_fsync(context->device_node);
}

int ext2_super_commit(ext2_vfs_context_t* restrict context)
{
    context->super.s_wtime = htole32((uint32_t)(wallclock_now()));

    return fcache_dev_write(context->device_node, EXT2_SUPER_OFFSET, &context->super, sizeof(ext2_superblock_t));
}

int ext2_group_commit(ext2_vfs_context_t* restrict context, uint32_t group)
{
    uint64_t offset = context->groups_offset + (uint64_t)(group) * sizeof(ext2_group_desc_t);
    return fcache_dev_write(context->device_node, offset, &context->groups[group], sizeof(ext2_group_desc_t));
}

static const vfs_ops_t ext2_vfs_ops = {
    .probe = &ext2_probe,
    .mount = &ext2_mount,
    .statvfs = &ext2_statvfs,
    .fill_node = &ext2_fill_node,
    .unmount = &ext2_unmount,
    .sync = &ext2_sync,
};

static vfs_driver_t ext2_vfs_driver = {
    .name = "ext2",
    .ops = &ext2_vfs_ops,
};

static void init_ext2(void)
{
    vfs_driver_add(&ext2_vfs_driver);
}

initcall_bsp_vfs(init_ext2, INIT_EARLY);
