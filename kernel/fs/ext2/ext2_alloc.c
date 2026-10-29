#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/kprintf.h>
#include <vex/string.h>

#include "ext2_internal.h"

static uint32_t group_blocks(const ext2_vfs_context_t* restrict context, uint32_t group)
{
    uint32_t first = context->first_data_block + group * context->blocks_per_group;
    uint32_t count = le32toh(context->super.s_blocks_count) - first;

    if(count > context->blocks_per_group) {
        count = context->blocks_per_group;
    }

    return count;
}

static bool find_clear_bit(const uint8_t* restrict bitmap, uint32_t start, uint32_t limit, uint32_t* restrict out)
{
    uint32_t bit = start;

    while(bit < limit) {
        uint8_t byte = bitmap[bit / 8];

        if(byte == 0xFF && 0 == bit % 8) {
            bit += 8;
            continue;
        }

        if(0 == (byte & (1U << (bit % 8)))) {
            *out = bit;
            return true;
        }

        bit += 1;
    }

    return false;
}

// Reads the bitmap a piece at a time, a whole block of it doesn't fit the slab
static int scan_bitmap(ext2_vfs_context_t* restrict context, uint64_t offset, uint32_t start, uint32_t limit, uint32_t* restrict out)
{
    uint8_t chunk[256];
    uint32_t bit = start;

    while(bit < limit) {
        uint32_t first_byte = bit / 8;
        uint32_t chunk_end = (first_byte + sizeof(chunk)) * 8;

        if(chunk_end > limit) {
            chunk_end = limit;
        }

        size_t bytes = (chunk_end + 7) / 8 - first_byte;
        int retval = fcache_dev_read(context->device_node, offset + first_byte, chunk, bytes);

        if(retval < 0) {
            return retval;
        }

        uint32_t found;

        if(find_clear_bit(chunk, bit - first_byte * 8, chunk_end - first_byte * 8, &found)) {
            *out = first_byte * 8 + found;
            return 0;
        }

        bit = chunk_end;
    }

    return -ENOSPC;
}

// Looks in [start, limit) first, then wraps around to [first, start)
static int bitmap_take(ext2_vfs_context_t* restrict context, uint32_t bitmap_block, uint32_t first, uint32_t start, uint32_t limit,
    uint32_t* restrict out)
{
    uint64_t offset = ext2_block_offset(context, bitmap_block);
    uint32_t bit;
    int retval = scan_bitmap(context, offset, start, limit, &bit);

    if(retval == -ENOSPC) {
        retval = scan_bitmap(context, offset, first, start, &bit);
    }

    if(retval < 0) {
        return retval;
    }

    uint8_t byte;
    retval = fcache_dev_read(context->device_node, offset + bit / 8, &byte, sizeof(byte));

    if(retval < 0) {
        return retval;
    }

    byte |= 1U << (bit % 8);
    retval = fcache_dev_write(context->device_node, offset + bit / 8, &byte, sizeof(byte));

    if(retval < 0) {
        return retval;
    }

    *out = bit;

    return 0;
}

static int bitmap_put(ext2_vfs_context_t* restrict context, uint32_t bitmap_block, uint32_t bit)
{
    uint64_t offset = ext2_block_offset(context, bitmap_block) + bit / 8;
    uint8_t byte;
    int retval = fcache_dev_read(context->device_node, offset, &byte, sizeof(byte));

    if(retval < 0) {
        return retval;
    }

    if(0 == (byte & (1U << (bit % 8)))) {
        return -EINVAL;
    }

    byte &= ~(1U << (bit % 8));

    return fcache_dev_write(context->device_node, offset, &byte, sizeof(byte));
}

static void commit_counts(ext2_vfs_context_t* restrict context, uint32_t group)
{
    int retval = ext2_group_commit(context, group);

    if(retval == 0) {
        retval = ext2_super_commit(context);
    }

    if(retval < 0) {
        pr_warning("ext2: %s: failed to update the free counts of group %u", context->volume->name, group);
    }
}

static uint32_t directory_group(const ext2_vfs_context_t* restrict context)
{
    uint32_t best = 0;
    uint32_t best_free = 0;

    for(uint32_t i = 0; i < context->groups_count; ++i) {
        const ext2_group_desc_t* desc = &context->groups[i];
        uint32_t free_blocks = le16toh(desc->bg_free_blocks_count);

        if(le16toh(desc->bg_free_inodes_count) && free_blocks > best_free) {
            best = i;
            best_free = free_blocks;
        }
    }

    return best;
}

uint32_t ext2_block_alloc(ext2_vfs_context_t* restrict context, uint32_t goal)
{
    ext2_superblock_t* super = &context->super;
    uint32_t free_blocks = le32toh(super->s_free_blocks_count);

    if(free_blocks == 0) {
        return 0;
    }

    if(goal < context->first_data_block || goal >= le32toh(super->s_blocks_count)) {
        goal = context->first_data_block;
    }

    uint32_t start_group = (goal - context->first_data_block) / context->blocks_per_group;
    uint32_t start_bit = (goal - context->first_data_block) % context->blocks_per_group;

    for(uint32_t i = 0; i < context->groups_count; ++i) {
        uint32_t group = (start_group + i) % context->groups_count;
        ext2_group_desc_t* desc = &context->groups[group];
        uint32_t group_free = le16toh(desc->bg_free_blocks_count);

        if(group_free == 0) {
            continue;
        }

        uint32_t start = 0;

        if(i == 0) {
            start = start_bit;
        }

        uint32_t bit;
        uint32_t bitmap_block = le32toh(desc->bg_block_bitmap);
        uint32_t limit = group_blocks(context, group);
        int retval = bitmap_take(context, bitmap_block, 0, start, limit, &bit);

        if(retval < 0) {
            pr_warning("ext2: %s: no block taken from group %u: %s", context->volume->name, group, strerror(retval));
            continue;
        }

        desc->bg_free_blocks_count = htole16(group_free - 1);
        super->s_free_blocks_count = htole32(free_blocks - 1);
        commit_counts(context, group);

        return context->first_data_block + group * context->blocks_per_group + bit;
    }

    return 0;
}

void ext2_block_free(ext2_vfs_context_t* restrict context, uint32_t block)
{
    ext2_superblock_t* super = &context->super;

    if(block < context->first_data_block || block >= le32toh(super->s_blocks_count)) {
        pr_warning("ext2: %s: freeing block %u outside the filesystem", context->volume->name, block);
        return;
    }

    uint32_t group = (block - context->first_data_block) / context->blocks_per_group;
    uint32_t bit = (block - context->first_data_block) % context->blocks_per_group;
    ext2_group_desc_t* desc = &context->groups[group];

    uint32_t bitmap_block = le32toh(desc->bg_block_bitmap);
    int retval = bitmap_put(context, bitmap_block, bit);

    if(retval < 0) {
        pr_warning("ext2: %s: failed to free block %u: %s", context->volume->name, block, strerror(retval));
        return;
    }

    desc->bg_free_blocks_count = htole16(le16toh(desc->bg_free_blocks_count) + 1);
    super->s_free_blocks_count = htole32(le32toh(super->s_free_blocks_count) + 1);
    commit_counts(context, group);
}

uint32_t ext2_inode_alloc(ext2_vfs_context_t* restrict context, uint32_t parent_ino, bool directory)
{
    ext2_superblock_t* super = &context->super;
    uint32_t free_inodes = le32toh(super->s_free_inodes_count);

    if(free_inodes == 0) {
        return 0;
    }

    uint32_t start_group = 0;

    if(directory) {
        start_group = directory_group(context);
    }
    else if(parent_ino && parent_ino <= le32toh(super->s_inodes_count)) {
        start_group = (parent_ino - 1) / context->inodes_per_group;
    }

    for(uint32_t i = 0; i < context->groups_count; ++i) {
        uint32_t group = (start_group + i) % context->groups_count;
        ext2_group_desc_t* desc = &context->groups[group];
        uint32_t group_free = le16toh(desc->bg_free_inodes_count);

        if(group_free == 0) {
            continue;
        }

        // Inodes below s_first_ino are
        // reserved, they all sit in group 0
        uint32_t first = 0;

        if(group == 0) {
            first = context->first_ino - 1;
        }

        uint32_t bit;
        uint32_t bitmap_block = le32toh(desc->bg_inode_bitmap);
        int retval = bitmap_take(context, bitmap_block, first, first, context->inodes_per_group, &bit);

        if(retval < 0) {
            pr_warning("ext2: %s: no inode taken from group %u: %s", context->volume->name, group, strerror(retval));
            continue;
        }

        uint32_t ino = group * context->inodes_per_group + bit + 1;

        desc->bg_free_inodes_count = htole16(group_free - 1);
        super->s_free_inodes_count = htole32(free_inodes - 1);

        if(directory) {
            desc->bg_used_dirs_count = htole16(le16toh(desc->bg_used_dirs_count) + 1);
        }

        commit_counts(context, group);

        if(ext2_inode_clear(context, ino) < 0) {
            pr_warning("ext2: %s: failed to clear inode %u", context->volume->name, ino);
        }

        return ino;
    }

    return 0;
}

void ext2_inode_free(ext2_vfs_context_t* restrict context, uint32_t ino, bool directory)
{
    ext2_superblock_t* super = &context->super;

    if(ino < context->first_ino || ino > le32toh(super->s_inodes_count)) {
        pr_warning("ext2: %s: freeing reserved or invalid inode %u", context->volume->name, ino);
        return;
    }

    uint32_t group = (ino - 1) / context->inodes_per_group;
    uint32_t bit = (ino - 1) % context->inodes_per_group;
    ext2_group_desc_t* desc = &context->groups[group];

    uint32_t bitmap_block = le32toh(desc->bg_inode_bitmap);
    int retval = bitmap_put(context, bitmap_block, bit);

    if(retval < 0) {
        pr_warning("ext2: %s: failed to free inode %u: %s", context->volume->name, ino, strerror(retval));
        return;
    }

    desc->bg_free_inodes_count = htole16(le16toh(desc->bg_free_inodes_count) + 1);
    super->s_free_inodes_count = htole32(le32toh(super->s_free_inodes_count) + 1);

    if(directory) {
        desc->bg_used_dirs_count = htole16(le16toh(desc->bg_used_dirs_count) - 1);
    }

    commit_counts(context, group);
}
