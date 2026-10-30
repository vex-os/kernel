#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/string.h>
#include <vex/vnode.h>
#include <vex/xattr.h>

#include "ext2_internal.h"

#define ENTRY_LEN(name_len) (size_t)((sizeof(ext2_xattr_entry_t) + (name_len) + 3) & ~(size_t)(3))
#define VALUE_LEN(size)     (size_t)(((size) + 3) & ~(size_t)(3))

#define NAME_HASH_SHIFT  5
#define VALUE_HASH_SHIFT 16
#define BLOCK_HASH_SHIFT 16

enum {
    AREA_IBODY,
    AREA_BLOCK,
    NUM_AREAS,
};

typedef struct ext2_area_s ext2_area_t;
typedef struct ext2_region_s ext2_region_t;

// Where attributes sit on the device; values count their offsets from base
struct ext2_area_s {
    uint64_t base;
    uint64_t first;
    uint64_t end;
};

// The same layout loaded into memory to change it
struct ext2_region_s {
    uint8_t* base;
    uint8_t* first;
    uint8_t* end;
};

static const uint8_t indices[NUM_XATTR_SPACES] = {
    [XATTR_USER] = EXT2_XATTR_INDEX_USER,
    [XATTR_TRUSTED] = EXT2_XATTR_INDEX_TRUSTED,
    [XATTR_SECURITY] = EXT2_XATTR_INDEX_SECURITY,
};

static bool index_of(unsigned space, uint8_t* restrict out)
{
    if(space >= NUM_XATTR_SPACES) {
        return false;
    }

    *out = indices[space];

    return true;
}

// ACLs and other indices Linux knows about stay hidden
static bool space_of(uint8_t index, unsigned* restrict out)
{
    for(unsigned space = 0; space < NUM_XATTR_SPACES; space += 1) {
        if(indices[space] == index) {
            *out = space;
            return true;
        }
    }

    return false;
}

static uint32_t rotate_xor(uint32_t hash, unsigned shift, uint32_t value)
{
    return (hash << shift) ^ (hash >> (32 - shift)) ^ value;
}

static uint32_t hash_entry(const uint8_t* restrict base, const ext2_xattr_entry_t* restrict entry)
{
    uint32_t hash = 0;

    // Linux hashes the name as plain char, which
    // is signed on x86 hence the cast MGE paravozik
    for(size_t i = 0; i < entry->e_name_len; ++i) {
        hash = rotate_xor(hash, NAME_HASH_SHIFT, (uint32_t)(int32_t)(int8_t)(entry->e_name[i]));
    }

    size_t size = VALUE_LEN(le32toh(entry->e_value_size));
    const uint8_t* value = base + le16toh(entry->e_value_offs);

    for(size_t i = 0; i < size; i += sizeof(uint32_t)) {
        uint32_t word;
        memcpy(&word, value + i, sizeof(word));
        hash = rotate_xor(hash, VALUE_HASH_SHIFT, le32toh(word));
    }

    return hash;
}

static bool is_last(const uint8_t* restrict at)
{
    uint32_t word;
    memcpy(&word, at, sizeof(word));
    return word == 0;
}

static int compare_entry(uint8_t index, const char* restrict name, size_t name_len, const ext2_xattr_entry_t* restrict entry)
{
    if(index != entry->e_name_index) {
        return (int)(index) - (int)(entry->e_name_index);
    }

    if(name_len != entry->e_name_len) {
        return (int)(name_len) - (int)(entry->e_name_len);
    }

    return memcmp(name, entry->e_name, name_len);
}

static int region_check(const ext2_region_t* restrict region, uint8_t** restrict out_last, size_t* restrict out_lowest)
{
    size_t lowest = region->end - region->base;
    uint8_t* at = region->first;

    for(;;) {
        if(at + sizeof(uint32_t) > region->end) {
            return -EIO;
        }

        if(is_last(at)) {
            break;
        }

        const ext2_xattr_entry_t* entry = (const ext2_xattr_entry_t*)(at);

        if(at + sizeof(ext2_xattr_entry_t) > region->end || at + ENTRY_LEN(entry->e_name_len) > region->end || entry->e_value_block) {
            return -EIO;
        }

        size_t size = le32toh(entry->e_value_size);
        size_t offset = le16toh(entry->e_value_offs);

        if(size && (offset % sizeof(uint32_t) || offset + VALUE_LEN(size) > (size_t)(region->end - region->base))) {
            return -EIO;
        }

        if(size && offset < lowest) {
            lowest = offset;
        }

        at += ENTRY_LEN(entry->e_name_len);
    }

    if(region->base + lowest < at + sizeof(uint32_t)) {
        return -EIO;
    }

    *out_last = at;
    *out_lowest = lowest;

    return 0;
}

static ext2_xattr_entry_t* region_find(const ext2_region_t* restrict region, uint8_t index, const char* restrict name)
{
    size_t name_len = strlen(name);

    for(uint8_t* at = region->first; !is_last(at);) {
        ext2_xattr_entry_t* entry = (ext2_xattr_entry_t*)(at);

        if(0 == compare_entry(index, name, name_len, entry)) {
            return entry;
        }

        at += ENTRY_LEN(entry->e_name_len);
    }

    return nullptr;
}

static void region_remove(const ext2_region_t* restrict region, ext2_xattr_entry_t* restrict removed, uint8_t* restrict last, size_t lowest)
{
    size_t value_len = VALUE_LEN(le32toh(removed->e_value_size));
    size_t value_offset = le16toh(removed->e_value_offs);

    if(value_len) {
        for(uint8_t* at = region->first; at < last;) {
            ext2_xattr_entry_t* entry = (ext2_xattr_entry_t*)(at);
            size_t offset = le16toh(entry->e_value_offs);

            if(entry != removed && entry->e_value_size && offset < value_offset) {
                entry->e_value_offs = htole16((uint16_t)(offset + value_len));
            }

            at += ENTRY_LEN(entry->e_name_len);
        }

        memmove(region->base + lowest + value_len, region->base + lowest, value_offset - lowest);
        memset(region->base + lowest, 0, value_len);
    }

    size_t entry_len = ENTRY_LEN(removed->e_name_len);
    uint8_t* next = (uint8_t*)(removed) + entry_len;

    memmove(removed, next, last + sizeof(uint32_t) - next);
    memset(last + sizeof(uint32_t) - entry_len, 0, entry_len);
}

static int region_insert(const ext2_region_t* restrict region, uint8_t index, const char* restrict name, const void* restrict value,
    size_t size)
{
    uint8_t* last;
    size_t lowest;
    int retval = region_check(region, &last, &lowest);

    if(retval < 0) {
        return retval;
    }

    size_t name_len = strlen(name);
    size_t entry_len = ENTRY_LEN(name_len);
    size_t value_len = VALUE_LEN(size);

    if(value_len > lowest || last + sizeof(uint32_t) + entry_len > region->base + lowest - value_len) {
        return -ENOSPC;
    }

    uint8_t* at = region->first;

    while(at < last) {
        const ext2_xattr_entry_t* entry = (const ext2_xattr_entry_t*)(at);

        if(compare_entry(index, name, name_len, entry) < 0) {
            break;
        }

        at += ENTRY_LEN(entry->e_name_len);
    }

    memmove(at + entry_len, at, last + sizeof(uint32_t) - at);
    memset(at, 0, entry_len);

    ext2_xattr_entry_t* entry = (ext2_xattr_entry_t*)(at);

    entry->e_name_len = (uint8_t)(name_len);
    entry->e_name_index = index;
    entry->e_value_size = htole32((uint32_t)(size));
    memcpy(entry->e_name, name, name_len);

    if(size) {
        size_t offset = lowest - value_len;

        memset(region->base + offset, 0, value_len);
        memcpy(region->base + offset, value, size);
        entry->e_value_offs = htole16((uint16_t)(offset));
    }

    entry->e_hash = htole32(hash_entry(region->base, entry));

    return 0;
}

static void rehash_block(const ext2_region_t* restrict region)
{
    ext2_xattr_header_t* header = (ext2_xattr_header_t*)(region->base);
    uint32_t hash = 0;

    for(uint8_t* at = region->first; !is_last(at);) {
        const ext2_xattr_entry_t* entry = (const ext2_xattr_entry_t*)(at);

        if(entry->e_hash == 0) {
            hash = 0;
            break;
        }

        hash = rotate_xor(hash, BLOCK_HASH_SHIFT, le32toh(entry->e_hash));
        at += ENTRY_LEN(entry->e_name_len);
    }

    header->h_hash = htole32(hash);
}

static int ibody_area(ext2_vfs_context_t* restrict fs_ctx, uint32_t ino, ext2_area_t* restrict out)
{
    if(fs_ctx->inode_size <= EXT2_GOOD_OLD_INODE_SIZE) {
        return -ENODATA;
    }

    uint64_t offset;
    int retval = ext2_inode_offset(fs_ctx, ino, &offset);

    if(retval < 0) {
        return retval;
    }

    uint16_t extra_isize;
    retval = fcache_dev_read(fs_ctx->device_node, offset + EXT2_GOOD_OLD_INODE_SIZE, &extra_isize, sizeof(extra_isize));

    if(retval < 0) {
        return retval;
    }

    size_t start = EXT2_GOOD_OLD_INODE_SIZE + le16toh(extra_isize);

    if(le16toh(extra_isize) < sizeof(extra_isize) || start % sizeof(uint32_t) || start + 2 * sizeof(uint32_t) > fs_ctx->inode_size) {
        return -ENODATA;
    }

    uint32_t magic;
    retval = fcache_dev_read(fs_ctx->device_node, offset + start, &magic, sizeof(magic));

    if(retval < 0) {
        return retval;
    }

    if(le32toh(magic) != EXT2_XATTR_MAGIC) {
        return -ENODATA;
    }

    out->base = offset + start + sizeof(magic);
    out->first = out->base;
    out->end = offset + fs_ctx->inode_size;

    return 0;
}

static int block_area(ext2_vfs_context_t* restrict fs_ctx, uint32_t block, ext2_area_t* restrict out)
{
    if(block == 0) {
        return -ENODATA;
    }

    if(block < fs_ctx->first_data_block || block >= le32toh(fs_ctx->super.s_blocks_count)) {
        return -EIO;
    }

    ext2_xattr_header_t header;
    uint64_t offset = ext2_block_offset(fs_ctx, block);
    int retval = fcache_dev_read(fs_ctx->device_node, offset, &header, sizeof(header));

    if(retval < 0) {
        return retval;
    }

    if(le32toh(header.h_magic) != EXT2_XATTR_MAGIC || le32toh(header.h_blocks) != 1) {
        pr_warning("ext2: %s: xattr block %u is broken", fs_ctx->volume->name, block);
        return -EIO;
    }

    out->base = offset;
    out->first = offset + sizeof(header);
    out->end = offset + fs_ctx->block_size;

    return 0;
}

static int node_area(vnode_t* restrict node, unsigned which, ext2_area_t* restrict out)
{
    ext2_vnode_context_t* ctx = node->ctxdata;

    if(which == AREA_IBODY) {
        return ibody_area(ctx->fs_ctx, (uint32_t)(node->ino), out);
    }

    uint32_t block = le32toh(ctx->inode.i_file_acl);

    return block_area(ctx->fs_ctx, block, out);
}

// Leaves the name NUL-terminated; returns zero past the last entry
static int area_entry(ext2_vfs_context_t* restrict fs_ctx, const ext2_area_t* restrict area, uint64_t at,
    ext2_xattr_entry_t* restrict entry, char* restrict name)
{
    uint32_t word;

    if(at + sizeof(word) > area->end) {
        return -EIO;
    }

    int retval = fcache_dev_read(fs_ctx->device_node, at, &word, sizeof(word));

    if(retval < 0) {
        return retval;
    }

    if(word == 0) {
        return 0;
    }

    if(at + sizeof(ext2_xattr_entry_t) > area->end) {
        return -EIO;
    }

    retval = fcache_dev_read(fs_ctx->device_node, at, entry, sizeof(ext2_xattr_entry_t));

    if(retval < 0) {
        return retval;
    }

    size_t size = le32toh(entry->e_value_size);

    if(at + ENTRY_LEN(entry->e_name_len) > area->end || entry->e_value_block) {
        return -EIO;
    }

    if(size && area->base + le16toh(entry->e_value_offs) + size > area->end) {
        return -EIO;
    }

    retval = fcache_dev_read(fs_ctx->device_node, at + sizeof(ext2_xattr_entry_t), name, entry->e_name_len);

    if(retval < 0) {
        return retval;
    }

    name[entry->e_name_len] = 0;

    return 1;
}

static int find_entry(vnode_t* restrict node, uint8_t index, const char* restrict name, unsigned* restrict out_which,
    ext2_area_t* restrict out_area, ext2_xattr_entry_t* restrict out_entry)
{
    ext2_vnode_context_t* ctx = node->ctxdata;

    for(unsigned which = 0; which < NUM_AREAS; which += 1) {
        ext2_area_t area;
        int retval = node_area(node, which, &area);

        if(retval == -ENODATA) {
            continue;
        }

        if(retval < 0) {
            return retval;
        }

        char found[XATTR_NAME_MAX + 1];

        for(uint64_t at = area.first;; at += ENTRY_LEN(out_entry->e_name_len)) {
            retval = area_entry(ctx->fs_ctx, &area, at, out_entry, found);

            if(retval < 0) {
                return retval;
            }

            if(retval == 0) {
                break;
            }

            if(out_entry->e_name_index == index && 0 == strcmp(found, name)) {
                *out_which = which;
                *out_area = area;
                return 0;
            }
        }
    }

    return -ENODATA;
}

// The goal is the start of the inode's group, close to the inode table
static uint32_t take_xattr_block(vnode_t* restrict node, uint32_t goal)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;

    if(goal == 0) {
        uint32_t group = ((uint32_t)(node->ino) - 1) / fs_ctx->inodes_per_group;
        goal = fs_ctx->first_data_block + group * fs_ctx->blocks_per_group;
    }

    return ext2_block_alloc(fs_ctx, goal);
}

static int enable_ext_attr(ext2_vfs_context_t* restrict fs_ctx)
{
    uint32_t compat = le32toh(fs_ctx->super.s_feature_compat);

    if(compat & EXT2_FEATURE_COMPAT_EXT_ATTR) {
        return 0;
    }

    fs_ctx->super.s_feature_compat = htole32(compat | EXT2_FEATURE_COMPAT_EXT_ATTR);

    return ext2_super_commit(fs_ctx);
}

// An emptied block goes away, and a block shared with other inodes is copied before it changes
static int store_block(vnode_t* restrict node, uint8_t* restrict buffer, uint32_t block, uint32_t refcount)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    ext2_inode_t* inode = &ctx->inode;
    uint32_t sectors = fs_ctx->block_size / EXT2_SECTOR_SIZE;

    if(is_last(buffer + sizeof(ext2_xattr_header_t))) {
        if(block) {
            ext2_xattr_release(fs_ctx, block);
            inode->i_file_acl = 0;
            inode->i_blocks = htole32(le32toh(inode->i_blocks) - sectors);
        }

        return 0;
    }

    if(block && refcount == 1) {
        uint64_t offset = ext2_block_offset(fs_ctx, block);

        return fcache_dev_write(fs_ctx->device_node, offset, buffer, fs_ctx->block_size);
    }

    uint32_t taken = take_xattr_block(node, block);

    if(taken == 0) {
        return -ENOSPC;
    }

    ext2_xattr_header_t* header = (ext2_xattr_header_t*)(buffer);
    header->h_refcount = htole32(1);

    uint64_t offset = ext2_block_offset(fs_ctx, taken);

    int retval = fcache_dev_write(fs_ctx->device_node, offset, buffer, fs_ctx->block_size);

    if(retval == 0) {
        retval = enable_ext_attr(fs_ctx);
    }

    if(retval < 0) {
        ext2_block_free(fs_ctx, taken);
        return retval;
    }

    if(block) {
        ext2_xattr_release(fs_ctx, block);
    }
    else {
        inode->i_blocks = htole32(le32toh(inode->i_blocks) + sectors);
    }

    inode->i_file_acl = htole32(taken);

    return 0;
}

static int change_block(vnode_t* restrict node, uint8_t index, const char* restrict name, const void* restrict value, size_t size,
    bool remove)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    uint32_t block = le32toh(ctx->inode.i_file_acl);
    uint8_t* buffer = kmalloc(fs_ctx->block_size);

    if(buffer == nullptr) {
        return -ENOMEM;
    }

    ext2_region_t region;
    region.base = buffer;
    region.first = buffer + sizeof(ext2_xattr_header_t);
    region.end = buffer + fs_ctx->block_size;

    ext2_xattr_header_t* header = (ext2_xattr_header_t*)(buffer);
    uint32_t refcount = 0;
    int retval = 0;

    if(block) {
        ext2_area_t area;
        retval = block_area(fs_ctx, block, &area);

        if(retval == 0) {
            retval = fcache_dev_read(fs_ctx->device_node, area.base, buffer, fs_ctx->block_size);
        }

        refcount = le32toh(header->h_refcount);
    }
    else {
        memset(buffer, 0, fs_ctx->block_size);
        header->h_magic = htole32(EXT2_XATTR_MAGIC);
        header->h_blocks = htole32(1);
    }

    uint8_t* last;
    size_t lowest;

    if(retval == 0) {
        retval = region_check(&region, &last, &lowest);
    }

    if(retval == 0) {
        ext2_xattr_entry_t* old = region_find(&region, index, name);

        if(old) {
            region_remove(&region, old, last, lowest);
        }
    }

    if(retval == 0 && !remove) {
        retval = region_insert(&region, index, name, value, size);
    }

    if(retval == 0) {
        rehash_block(&region);
        retval = store_block(node, buffer, block, refcount);
    }

    kfree(buffer);

    return retval;
}

static int remove_from_ibody(vnode_t* restrict node, uint8_t index, const char* restrict name)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    ext2_area_t area;
    int retval = ibody_area(fs_ctx, (uint32_t)(node->ino), &area);

    if(retval < 0) {
        return retval;
    }

    size_t length = area.end - area.first;
    uint8_t* buffer = kmalloc(length);

    if(buffer == nullptr) {
        return -ENOMEM;
    }

    ext2_region_t region;
    region.base = buffer;
    region.first = buffer;
    region.end = buffer + length;

    retval = fcache_dev_read(fs_ctx->device_node, area.first, buffer, length);

    uint8_t* last;
    size_t lowest;

    if(retval == 0) {
        retval = region_check(&region, &last, &lowest);
    }

    if(retval == 0) {
        ext2_xattr_entry_t* old = region_find(&region, index, name);

        if(old) {
            region_remove(&region, old, last, lowest);
            retval = fcache_dev_write(fs_ctx->device_node, area.first, buffer, length);
        }
    }

    kfree(buffer);

    return retval;
}

ssize_t ext2_xattr_get(vnode_t* restrict node, unsigned space, const char* restrict name, void* restrict value, size_t size)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    uint8_t index;

    if(!index_of(space, &index)) {
        return -EOPNOTSUPP;
    }

    unsigned which;
    ext2_area_t area;
    ext2_xattr_entry_t entry;
    int retval = find_entry(node, index, name, &which, &area, &entry);

    if(retval < 0) {
        return retval;
    }

    size_t length = le32toh(entry.e_value_size);

    if(size == 0) {
        return length;
    }

    if(length > size) {
        return -ERANGE;
    }

    if(length) {
        uint64_t offset = area.base + le16toh(entry.e_value_offs);

        retval = fcache_dev_read(ctx->fs_ctx->device_node, offset, value, length);
    }

    if(retval < 0) {
        return retval;
    }

    return length;
}

int ext2_xattr_iterate(vnode_t* restrict node, xattr_entry_t* restrict out, size_t index)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    size_t visible = 0;

    for(unsigned which = 0; which < NUM_AREAS; which += 1) {
        ext2_area_t area;
        int retval = node_area(node, which, &area);

        if(retval == -ENODATA) {
            continue;
        }

        if(retval < 0) {
            return retval;
        }

        ext2_xattr_entry_t entry;

        for(uint64_t at = area.first;; at += ENTRY_LEN(entry.e_name_len)) {
            retval = area_entry(ctx->fs_ctx, &area, at, &entry, out->name);

            if(retval < 0) {
                return retval;
            }

            if(retval == 0) {
                break;
            }

            if(!space_of(entry.e_name_index, &out->space)) {
                continue;
            }

            if(visible == index) {
                return 0;
            }

            visible += 1;
        }
    }

    return -ENOENT;
}

int ext2_xattr_set(vnode_t* restrict node, unsigned space, const char* restrict name, const void* restrict value, size_t size,
    unsigned flags)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    uint8_t index;

    if(!index_of(space, &index)) {
        return -EOPNOTSUPP;
    }

    unsigned which;
    ext2_area_t area;
    ext2_xattr_entry_t entry;
    int found = find_entry(node, index, name, &which, &area, &entry);

    if(found < 0 && found != -ENODATA) {
        return found;
    }

    if(found == 0 && (flags & XATTR_CREATE)) {
        return -EEXIST;
    }

    if(found == -ENODATA && (flags & XATTR_REPLACE)) {
        return -ENODATA;
    }

    int retval = change_block(node, index, name, value, size, false);

    if(retval == 0 && found == 0 && which == AREA_IBODY) {
        retval = remove_from_ibody(node, index, name);
    }

    if(retval == 0) {
        retval = ext2_inode_write(ctx->fs_ctx, (uint32_t)(node->ino), &ctx->inode);
    }

    return retval;
}

int ext2_xattr_remove(vnode_t* restrict node, unsigned space, const char* restrict name)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    uint8_t index;

    if(!index_of(space, &index)) {
        return -EOPNOTSUPP;
    }

    unsigned which;
    ext2_area_t area;
    ext2_xattr_entry_t entry;
    int retval = find_entry(node, index, name, &which, &area, &entry);

    if(retval == 0 && which == AREA_IBODY) {
        retval = remove_from_ibody(node, index, name);
    }
    else if(retval == 0) {
        retval = change_block(node, index, name, nullptr, 0, true);
    }

    if(retval == 0) {
        retval = ext2_inode_write(ctx->fs_ctx, (uint32_t)(node->ino), &ctx->inode);
    }

    return retval;
}

void ext2_xattr_release(ext2_vfs_context_t* restrict context, uint32_t block)
{
    const char* name = context->volume->name;

    if(block < context->first_data_block || block >= le32toh(context->super.s_blocks_count)) {
        pr_warning("ext2: %s: xattr block %u is outside the filesystem", name, block);
        return;
    }

    ext2_xattr_header_t header;
    uint64_t offset = ext2_block_offset(context, block);
    int retval = fcache_dev_read(context->device_node, offset, &header, sizeof(header));

    if(retval == 0 && le32toh(header.h_magic) != EXT2_XATTR_MAGIC) {
        retval = -EIO;
    }

    if(retval < 0) {
        pr_warning("ext2: %s: failed to read xattr block %u: %s", name, block, strerror(retval));
        return;
    }

    uint32_t refcount = le32toh(header.h_refcount);

    if(refcount > 1) {
        header.h_refcount = htole32(refcount - 1);

        if(fcache_dev_write(context->device_node, offset, &header, sizeof(header)) < 0) {
            pr_warning("ext2: %s: failed to drop a reference to xattr block %u", name, block);
        }

        return;
    }

    fcache_forget(context->device_node, offset, context->block_size);
    ext2_block_free(context, block);
}
