#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/kprintf.h>
#include <vex/mount.h>
#include <vex/mutex.h>
#include <vex/pslab.h>
#include <vex/slab.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/vcache.h>
#include <vex/vnode.h>
#include <vex/wallclock.h>

#include "ext2_internal.h"

static mutex_t* fs_lock(vnode_t* restrict node)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    return &ctx->fs_ctx->lock;
}

static bool is_fast_symlink(const ext2_vfs_context_t* restrict fs_ctx, const ext2_inode_t* restrict inode)
{
    uint32_t xattr_sectors = 0;

    if(inode->i_file_acl) {
        xattr_sectors = fs_ctx->block_size / 512;
    }

    return xattr_sectors == le32toh(inode->i_blocks);
}

static int entry_nodetype(ext2_vfs_context_t* restrict fs_ctx, const ext2_dirent_t* restrict entry, unsigned* restrict out)
{
    switch(entry->file_type) {
        case EXT2_FT_REG_FILE:
            *out = VNODE_FILE;
            return 0;

        case EXT2_FT_DIR:
            *out = VNODE_DIRECTORY;
            return 0;

        case EXT2_FT_SYMLINK:
            *out = VNODE_SYM_LINK;
            return 0;

        case EXT2_FT_UNKNOWN:
            break;

        default:
            *out = VNODE_FILE; // No vnode types for devices, FIFOs and sockets yet
            return 0;
    }

    ext2_inode_t inode;
    int retval = ext2_inode_read(fs_ctx, entry->ino, &inode);

    if(retval < 0) {
        return retval;
    }

    uint16_t mode = le16toh(inode.i_mode);

    if(ext2_inode_nodetype(mode, out) < 0) {
        *out = VNODE_FILE;
    }

    return 0;
}

static int lookup_locked(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    if(node->type != VNODE_DIRECTORY) {
        return -ENOTDIR;
    }

    if(strlen(name) > EXT2_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    ext2_dirent_t entry;
    int retval = ext2_dirent_find(node, name, &entry);

    if(retval < 0) {
        return retval;
    }

    *out = entry.ino;

    return 0;
}

static int iterate_locked(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_dirent_t entry;
    dirent_t record;
    uint64_t offset = dir_context->position;

    int retval = ext2_dirent_align(node, &offset);

    if(retval < 0) {
        return retval;
    }

    for(;;) {
        retval = ext2_dirent_read(node, &offset, &entry);

        if(retval == -ENOENT) {
            return 0;
        }

        if(retval < 0) {
            return retval;
        }

        unsigned type;
        retval = entry_nodetype(ctx->fs_ctx, &entry, &type);

        if(retval < 0) {
            return retval;
        }

        record.d_ino = entry.ino;
        record.d_type = dirent_type_from_vnode(type);

        memset(record.d_name, 0, NAME_SIZE);
        kstrncpy(record.d_name, entry.name, NAME_SIZE);

        if(!dir_emit(dir_context, &record, offset)) {
            return 0;
        }
    }
}

static ssize_t readlink_locked(vnode_t* restrict node, char* restrict buf, size_t count)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    const ext2_inode_t* inode = &ctx->inode;
    uint64_t size = ext2_inode_size(inode);

    if(node->type != VNODE_SYM_LINK) {
        return -EINVAL;
    }

    if(count > size) {
        count = size;
    }

    if(!is_fast_symlink(ctx->fs_ctx, inode)) {
        return fcache_read(node, &ext2_bmap, buf, count, 0, size);
    }

    if(size > sizeof(inode->i_block)) {
        return -EIO;
    }

    memcpy(buf, inode->i_block, count);

    return count;
}

static int write_inode_locked(vnode_t* restrict node)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    return ext2_inode_write(ctx->fs_ctx, (uint32_t)(node->ino), &ctx->inode);
}

// The new times reach the disk with the next inode write
static void touch_locked(vnode_t* restrict node, bool modified)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    timespec_t now;
    wallclock_get(&now);

    ext2_inode_set_time(&ctx->inode, EXT2_CTIME, &now);

    if(modified) {
        ext2_inode_set_time(&ctx->inode, EXT2_MTIME, &now);
    }
}

static void touch_dir_locked(vnode_t* restrict dir)
{
    touch_locked(dir, true);
    write_inode_locked(dir);
}

static bool block_mapped_locked(vnode_t* restrict node, uint64_t pos)
{
    fcache_extent_t extent;
    size_t sector_size = node->fs->volume->sector_size;
    return ext2_bmap(node, pos / sector_size, false, &extent) == 0 && extent.mapped;
}

static ssize_t write_span_locked(vnode_t* restrict node, const void* restrict buffer, size_t count, uint64_t offset)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    uint64_t size = ext2_inode_size(&ctx->inode);
    ssize_t written = fcache_write(node, &ext2_bmap, buffer, count, offset, size);

    if(written <= 0 || offset + written <= size) {
        return written;
    }

    int retval = ext2_inode_set_size(ctx->fs_ctx, &ctx->inode, offset + written);

    if(retval == 0) {
        retval = write_inode_locked(node);
    }

    if(retval < 0) {
        return retval;
    }

    return written;
}

static int zero_fill_locked(vnode_t* restrict node, uint64_t from, uint64_t to)
{
    void* zeroes = pslab_alloc_virt();

    if(zeroes == nullptr) {
        return -ENOMEM;
    }

    memset(zeroes, 0, PAGE_SIZE);

    int retval = 0;

    while(from < to) {
        size_t chunk = to - from;

        if(chunk > PAGE_SIZE) {
            chunk = PAGE_SIZE;
        }

        ssize_t written = write_span_locked(node, zeroes, chunk, from);

        if(written < 0) {
            retval = written;
            break;
        }

        from += written;
    }

    pslab_free_virt(zeroes);

    return retval;
}

static int fill_hole_locked(vnode_t* restrict node, uint64_t pos, uint64_t limit)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    size_t block_size = ctx->fs_ctx->block_size;

    if(block_mapped_locked(node, pos)) {
        return 0;
    }

    uint64_t start = pos - pos % block_size;
    uint64_t stop = start + block_size;

    if(stop > limit) {
        stop = limit;
    }

    return zero_fill_locked(node, start, stop);
}

static int clear_tail_locked(vnode_t* restrict node, uint64_t size, uint64_t new_size)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    size_t block_size = ctx->fs_ctx->block_size;

    if(0 == size % block_size || !block_mapped_locked(node, size)) {
        return 0;
    }

    uint64_t stop = size - size % block_size + block_size;

    if(stop > new_size) {
        stop = new_size;
    }

    return zero_fill_locked(node, size, stop);
}

static ssize_t write_locked(vnode_t* restrict node, const void* restrict buffer, size_t count, off_t offset)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    size_t block_size = ctx->fs_ctx->block_size;

    if(offset < 0) {
        return -EINVAL;
    }

    if(count == 0) {
        return 0;
    }

    uint64_t size = ext2_inode_size(&ctx->inode);
    uint64_t end = offset + count;
    uint64_t limit = size;

    if(end > limit) {
        limit = end;
    }

    int retval = 0;

    if((uint64_t)(offset) > size) {
        retval = clear_tail_locked(node, size, offset);
    }

    if(retval == 0) {
        retval = fill_hole_locked(node, offset, limit);
    }

    if(retval == 0 && (end - 1) / block_size != (uint64_t)(offset) / block_size) {
        retval = fill_hole_locked(node, end - 1, limit);
    }

    if(retval < 0) {
        return retval;
    }

    ssize_t written = write_span_locked(node, buffer, count, offset);

    if(written > 0) {
        touch_locked(node, true);
        retval = write_inode_locked(node);
    }

    if(retval < 0) {
        return retval;
    }

    return written;
}

static int resize_locked(vnode_t* restrict node, uint64_t new_size)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    uint64_t size = ext2_inode_size(&ctx->inode);
    int retval = 0;

    if(new_size > size) {
        retval = clear_tail_locked(node, size, new_size);
    }
    else if(new_size < size) {
        fcache_truncate(node, new_size);

        retval = ext2_truncate_blocks(node, new_size);
    }

    if(retval == 0) {
        retval = ext2_inode_set_size(ctx->fs_ctx, &ctx->inode, new_size);
    }

    if(retval == 0) {
        retval = write_inode_locked(node);
    }

    return retval;
}

static int set_attr_locked(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    ext2_inode_t* inode = &ctx->inode;

    if(attr->mask & (VATTR_UID | VATTR_GID)) {
        uid_t uid = ext2_inode_uid(fs_ctx, inode);
        gid_t gid = ext2_inode_gid(fs_ctx, inode);

        if(attr->mask & VATTR_UID) {
            uid = attr->uid;
        }

        if(attr->mask & VATTR_GID) {
            gid = attr->gid;
        }

        if(!ext2_inode_set_owner(fs_ctx, inode, uid, gid)) {
            return -EINVAL;
        }
    }

    if(attr->mask & VATTR_MODE) {
        uint16_t mode = le16toh(inode->i_mode);
        inode->i_mode = htole16((mode & S_IFMT) | (attr->mode & ~S_IFMT));
    }

    if(attr->mask & VATTR_ATIME) {
        ext2_inode_set_time(inode, EXT2_ATIME, &attr->atime);
    }

    if(attr->mask & VATTR_MTIME) {
        ext2_inode_set_time(inode, EXT2_MTIME, &attr->mtime);
    }

    if(attr->mask & VATTR_CTIME) {
        ext2_inode_set_time(inode, EXT2_CTIME, &attr->ctime);
    }

    if(attr->mask & VATTR_SIZE) {
        return resize_locked(node, attr->size);
    }

    if(attr->mask) {
        return write_inode_locked(node);
    }

    return 0;
}

static uint8_t file_type_of(uint16_t mode)
{
    switch(mode & S_IFMT) {
        case S_IFREG:
            return EXT2_FT_REG_FILE;

        case S_IFDIR:
            return EXT2_FT_DIR;

        case S_IFLNK:
            return EXT2_FT_SYMLINK;

        default:
            return EXT2_FT_UNKNOWN;
    }
}

static int add_links_locked(vnode_t* restrict node, int delta)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    int links = le16toh(ctx->inode.i_links_count) + delta;

    if(links < 0) {
        links = 0;
    }

    ctx->inode.i_links_count = htole16((uint16_t)(links));

    return write_inode_locked(node);
}

static void destroy_inode_locked(vnode_t* restrict node)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    ext2_inode_t* inode = &ctx->inode;
    uint32_t ino = (uint32_t)(node->ino);

    if(node->type != VNODE_SYM_LINK || !is_fast_symlink(fs_ctx, inode)) {
        if(ext2_truncate_blocks(node, 0) < 0) {
            pr_warning("ext2: %s: failed to free the blocks of inode %u", fs_ctx->volume->name, ino);
        }
    }

    uint32_t xattr_block = le32toh(inode->i_file_acl);

    if(xattr_block) {
        ext2_xattr_release(fs_ctx, xattr_block);
        inode->i_file_acl = 0;
    }

    ext2_inode_set_size(fs_ctx, inode, 0);
    inode->i_dtime = htole32((uint32_t)(wallclock_now()));
    write_inode_locked(node);
    ext2_inode_free(fs_ctx, ino, node->type == VNODE_DIRECTORY);
}

static int write_link_target_locked(vnode_t* restrict node, const char* restrict target)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    size_t length = strlen(target);
    int retval = 0;

    if(length < sizeof(ctx->inode.i_block)) {
        memcpy(ctx->inode.i_block, target, length);
    }
    else {
        ssize_t written = fcache_write(node, &ext2_bmap, target, length, 0, 0);

        if(written < 0) {
            retval = written;
        }
        else if((size_t)(written) != length) {
            retval = -EIO;
        }
    }

    if(retval == 0) {
        retval = ext2_inode_set_size(fs_ctx, &ctx->inode, length);
    }

    if(retval == 0) {
        retval = write_inode_locked(node);
    }

    return retval;
}

static bool link_target_fits(const vnode_t* restrict node, const char* restrict target)
{
    const ext2_vnode_context_t* ctx = node->ctxdata;

    return strlen(target) < ctx->fs_ctx->block_size;
}

static int link_locked(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    ext2_vnode_context_t* target_ctx = target->ctxdata;

    uint16_t links = le16toh(target_ctx->inode.i_links_count);

    if(strlen(name) > EXT2_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    // Unlinked while still open
    if(links == 0) {
        return -ENOENT;
    }

    if(links >= EXT2_LINK_MAX) {
        return -EMLINK;
    }

    ext2_dirent_t existing;
    int retval = ext2_dirent_find(node, name, &existing);

    if(retval == 0) {
        return -EEXIST;
    }

    if(retval != -ENOENT) {
        return retval;
    }

    uint16_t mode = le16toh(target_ctx->inode.i_mode);
    uint8_t file_type = file_type_of(mode);

    retval = ext2_dirent_add(node, name, (uint32_t)(target->ino), file_type);

    if(retval < 0) {
        return retval;
    }

    touch_dir_locked(node);
    touch_locked(target, false);

    return add_links_locked(target, 1);
}

static int create_locked(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, const char* restrict link_target,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    bool directory = type == VNODE_DIRECTORY;

    *out = nullptr;

    if(node->type != VNODE_DIRECTORY) {
        return -ENOTDIR;
    }

    if(strlen(name) > EXT2_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    if(type != VNODE_FILE && type != VNODE_DIRECTORY && type != VNODE_SYM_LINK) {
        return -EINVAL;
    }

    if(directory && le16toh(ctx->inode.i_links_count) >= EXT2_LINK_MAX) {
        return -EMLINK;
    }

    ext2_dirent_t existing;
    int retval = ext2_dirent_find(node, name, &existing);

    if(retval == 0) {
        return -EEXIST;
    }

    if(retval != -ENOENT) {
        return retval;
    }

    uint16_t inode_mode = mode & ~S_IFMT;
    gid_t gid = credentials->egid;

    if(directory) {
        inode_mode |= S_IFDIR;
    }
    else if(type == VNODE_SYM_LINK) {
        inode_mode |= S_IFLNK;
    }
    else {
        inode_mode |= S_IFREG;
    }

    // A set-group-ID directory hands down its
    // group, and the bit itself to subdirectories
    if(le16toh(ctx->inode.i_mode) & S_ISGID) {
        gid = ext2_inode_gid(fs_ctx, &ctx->inode);

        if(directory) {
            inode_mode |= S_ISGID;
        }
    }

    ext2_vnode_context_t* child_ctx = slab_alloc(sizeof(ext2_vnode_context_t));

    if(child_ctx == nullptr) {
        return -ENOMEM;
    }

    memset(child_ctx, 0, sizeof(ext2_vnode_context_t));

    child_ctx->fs_ctx = fs_ctx;
    child_ctx->inode.i_mode = htole16(inode_mode);
    child_ctx->inode.i_links_count = htole16(1);

    if(fs_ctx->inode_size > EXT2_GOOD_OLD_INODE_SIZE) {
        child_ctx->inode.i_extra_isize = htole16(EXT2_EXTRA_ISIZE);
    }

    timespec_t now;
    wallclock_get(&now);

    ext2_inode_set_time(&child_ctx->inode, EXT2_ATIME, &now);
    ext2_inode_set_time(&child_ctx->inode, EXT2_CTIME, &now);
    ext2_inode_set_time(&child_ctx->inode, EXT2_MTIME, &now);
    ext2_inode_set_time(&child_ctx->inode, EXT2_CRTIME, &now);

    if(directory) {
        child_ctx->inode.i_links_count = htole16(2);
    }

    if(!ext2_inode_set_owner(fs_ctx, &child_ctx->inode, credentials->euid, gid)) {
        slab_free(child_ctx);
        return -EINVAL;
    }

    uint32_t ino = ext2_inode_alloc(fs_ctx, (uint32_t)(node->ino), directory);

    if(ino == 0) {
        slab_free(child_ctx);
        return -ENOSPC;
    }

    vnode_t* child = vnode_alloc(type, &ext2_vnode_ops, child_ctx);

    if(child == nullptr) {
        ext2_inode_free(fs_ctx, ino, directory);
        slab_free(child_ctx);
        return -ENOMEM;
    }

    child->fs = node->fs;
    child->ino = ino;

    *out = child;

    retval = write_inode_locked(child);

    if(retval == 0 && directory) {
        retval = ext2_dir_init(child, (uint32_t)(node->ino));
    }

    if(retval == 0 && type == VNODE_SYM_LINK) {
        retval = write_link_target_locked(child, link_target);
    }

    // The new directory's ".." links to the parent
    if(retval == 0 && directory) {
        retval = add_links_locked(node, 1);
    }

    if(retval == 0) {
        retval = ext2_dirent_add(node, name, ino, file_type_of(inode_mode));

        if(retval < 0 && directory) {
            add_links_locked(node, -1);
        }
    }

    if(retval == 0) {
        touch_dir_locked(node);
    }

    if(retval < 0) {
        child_ctx->inode.i_links_count = 0;
    }

    return retval;
}

static int find_entry_locked(vnode_t* restrict dir, const char* restrict name, const vnode_t* restrict expected,
    ext2_dirent_t* restrict out)
{
    int retval = ext2_dirent_find(dir, name, out);

    if(retval < 0) {
        return retval;
    }

    if(out->ino != expected->ino) {
        return -ENOENT;
    }

    return 0;
}

static int drop_entry_target_locked(vnode_t* restrict target)
{
    ext2_vnode_context_t* ctx = target->ctxdata;
    int retval;

    if(target->type == VNODE_DIRECTORY) {
        ctx->inode.i_links_count = 0;
        retval = write_inode_locked(target);
    }
    else {
        retval = add_links_locked(target, -1);
    }

    // A file with names left stays cached
    // so they all keep leading to this vnode
    if(0 == ctx->inode.i_links_count) {
        vcache_remove(target);
    }

    return retval;
}

static int unlink_locked(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    ext2_dirent_t entry;
    int retval = find_entry_locked(node, name, target, &entry);

    if(retval == 0 && target->type == VNODE_DIRECTORY) {
        retval = ext2_dir_check_empty(target);
    }

    if(retval == 0) {
        retval = ext2_dirent_remove(node, entry.offset);
    }

    if(retval < 0) {
        return retval;
    }

    touch_dir_locked(node);

    // The removed directory's ".." linked to the parent
    if(target->type == VNODE_DIRECTORY) {
        add_links_locked(node, -1);
    }

    touch_locked(target, false);

    return drop_entry_target_locked(target);
}

static int rename_locked(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim)
{
    if(strlen(newname) > EXT2_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    ext2_dirent_t old_entry;
    int retval = find_entry_locked(olddir, oldname, source, &old_entry);

    if(retval < 0) {
        return retval;
    }

    ext2_dirent_t new_entry;
    retval = ext2_dirent_find(newdir, newname, &new_entry);

    if(retval < 0 && retval != -ENOENT) {
        return retval;
    }

    if(victim == nullptr && retval == 0) {
        return -EEXIST;
    }

    if(victim && (retval == -ENOENT || new_entry.ino != victim->ino)) {
        return -ENOENT;
    }

    ext2_vnode_context_t* source_ctx = source->ctxdata;
    ext2_vnode_context_t* newdir_ctx = newdir->ctxdata;
    bool victim_directory = victim && victim->type == VNODE_DIRECTORY;
    bool moves_directory = source->type == VNODE_DIRECTORY && olddir != newdir;

    if(victim_directory) {
        retval = ext2_dir_check_empty(victim);

        if(retval < 0) {
            return retval;
        }
    }

    if(moves_directory && !victim_directory && le16toh(newdir_ctx->inode.i_links_count) >= EXT2_LINK_MAX) {
        return -EMLINK;
    }

    uint32_t ino = (uint32_t)(source->ino);
    uint8_t file_type = file_type_of(le16toh(source_ctx->inode.i_mode));

    if(victim) {
        retval = ext2_dirent_retarget(newdir, new_entry.offset, ino, file_type);
    }
    else {
        retval = ext2_dirent_add(newdir, newname, ino, file_type);
    }

    // Adding a record only ever splits others
    // so the old one is still where it was found
    if(retval == 0) {
        retval = ext2_dirent_remove(olddir, old_entry.offset);
    }

    if(retval < 0) {
        return retval;
    }

    touch_dir_locked(olddir);

    if(newdir != olddir) {
        touch_dir_locked(newdir);
    }

    touch_locked(source, false);
    write_inode_locked(source);

    // Links to a directory come from the ".." of its subdirectories
    if(moves_directory) {
        ext2_dirent_t dotdot;
        retval = ext2_dirent_find(source, "..", &dotdot);

        if(retval == 0) {
            retval = ext2_dirent_retarget(source, dotdot.offset, (uint32_t)(newdir->ino), EXT2_FT_DIR);
        }

        add_links_locked(olddir, -1);
        add_links_locked(newdir, 1);
    }

    if(victim_directory) {
        add_links_locked(newdir, -1);
    }

    if(victim) {
        touch_locked(victim, false);

        int victim_retval = drop_entry_target_locked(victim);

        if(retval == 0) {
            retval = victim_retval;
        }
    }

    return retval;
}

static void ext2_vnode_release(vnode_t* restrict node)
{
    ext2_vnode_context_t* ctx = node->ctxdata;

    if(0 == ctx->inode.i_links_count && !node->fs->readonly) {
        mutex_acquire(&ctx->fs_ctx->lock);
        destroy_inode_locked(node);
        mutex_release(&ctx->fs_ctx->lock);
    }

    slab_free(ctx);
}

static int ext2_vnode_lookup(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    mutex_acquire(fs_lock(node));
    int retval = lookup_locked(node, name, out);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_iterate(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    mutex_acquire(fs_lock(node));
    int retval = iterate_locked(node, dir_context);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_create(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    vnode_t* created;

    mutex_acquire(fs_lock(node));
    int retval = create_locked(node, name, type, mode, nullptr, credentials, &created);
    mutex_release(fs_lock(node));

    if(retval < 0 && created) {
        vnode_decref(created);
    }

    if(retval == 0) {
        *out = created;
    }

    return retval;
}

static int ext2_vnode_link(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    mutex_acquire(fs_lock(node));
    int retval = link_locked(node, name, target);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_symlink(vnode_t* restrict node, const char* restrict name, const char* restrict target,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    vnode_t* created;

    if(!link_target_fits(node, target)) {
        return -ENAMETOOLONG;
    }

    mutex_acquire(fs_lock(node));
    int retval = create_locked(node, name, VNODE_SYM_LINK, S_IRWXU | S_IRWXG | S_IRWXO, target, credentials, &created);
    mutex_release(fs_lock(node));

    if(retval < 0 && created) {
        vnode_decref(created);
    }

    if(retval == 0) {
        *out = created;
    }

    return retval;
}

static int ext2_vnode_unlink(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    mutex_acquire(fs_lock(node));
    int retval = unlink_locked(node, name, target);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_rename(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim)
{
    mutex_acquire(fs_lock(olddir));
    int retval = rename_locked(olddir, oldname, source, newdir, newname, victim);
    mutex_release(fs_lock(olddir));
    return retval;
}

static ssize_t ext2_vnode_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    ext2_vnode_context_t* ctx = node->ctxdata;

    if(node->type == VNODE_DIRECTORY) {
        return -EISDIR;
    }

    if(node->type != VNODE_FILE) {
        return -EINVAL;
    }

    mutex_acquire(fs_lock(node));
    ssize_t retval = fcache_read(node, &ext2_bmap, buffer, size, offset, ext2_inode_size(&ctx->inode));
    mutex_release(fs_lock(node));

    return retval;
}

static ssize_t ext2_vnode_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    if(node->type == VNODE_DIRECTORY) {
        return -EISDIR;
    }

    if(node->type != VNODE_FILE) {
        return -EINVAL;
    }

    mutex_acquire(fs_lock(node));
    ssize_t retval = write_locked(node, buffer, size, offset);
    mutex_release(fs_lock(node));

    return retval;
}

static ssize_t ext2_vnode_readlink(vnode_t* restrict node, char* restrict buf, size_t size)
{
    mutex_acquire(fs_lock(node));
    ssize_t retval = readlink_locked(node, buf, size);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_stat(vnode_t* restrict node, stat_t* restrict out)
{
    ext2_vnode_context_t* ctx = node->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    const ext2_inode_t* inode = &ctx->inode;

    memset(out, 0, sizeof(stat_t));

    mutex_acquire(&fs_ctx->lock);

    out->st_ino = node->ino;
    out->st_mode = le16toh(inode->i_mode);
    out->st_nlink = le16toh(inode->i_links_count);
    out->st_uid = ext2_inode_uid(fs_ctx, inode);
    out->st_gid = ext2_inode_gid(fs_ctx, inode);
    out->st_size = (off_t)(ext2_inode_size(inode));
    ext2_inode_get_time(inode, EXT2_ATIME, &out->st_atim);
    ext2_inode_get_time(inode, EXT2_MTIME, &out->st_mtim);
    ext2_inode_get_time(inode, EXT2_CTIME, &out->st_ctim);
    out->st_blksize = fs_ctx->block_size;
    out->st_blocks = le32toh(inode->i_blocks);

    mutex_release(&fs_ctx->lock);

    return 0;
}

static int ext2_vnode_set_attr(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    mutex_acquire(fs_lock(node));
    int retval = set_attr_locked(node, attr);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_fsync(vnode_t* restrict node)
{
    ext2_vnode_context_t* ctx = node->ctxdata;

    mutex_acquire(fs_lock(node));
    int retval = fcache_writeback(node, &ext2_bmap, ext2_inode_size(&ctx->inode));
    mutex_release(fs_lock(node));

    return retval;
}

static int ext2_vnode_fault(vnode_t* restrict node, off_t offset, pmap_t* restrict pmap, virt_addr_t virt, unsigned vprot)
{
    ext2_vnode_context_t* ctx = node->ctxdata;

    if(node->type != VNODE_FILE) {
        return -ENODEV;
    }

    mutex_acquire(fs_lock(node));
    uint64_t size = ext2_inode_size(&ctx->inode);

    int retval = fcache_fault(node, &ext2_bmap, offset, size, pmap, virt, vprot);
    mutex_release(fs_lock(node));

    return retval;
}

static ssize_t ext2_vnode_get_xattr(vnode_t* restrict node, unsigned space, const char* restrict name, void* restrict value, size_t size)
{
    mutex_acquire(fs_lock(node));
    ssize_t retval = ext2_xattr_get(node, space, name, value, size);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_set_xattr(vnode_t* restrict node, unsigned space, const char* restrict name, const void* restrict value, size_t size,
    unsigned flags)
{
    mutex_acquire(fs_lock(node));

    int retval = ext2_xattr_set(node, space, name, value, size, flags);

    if(retval == 0) {
        touch_locked(node, false);
        retval = write_inode_locked(node);
    }

    mutex_release(fs_lock(node));

    return retval;
}

static int ext2_vnode_iterate_xattr(vnode_t* restrict node, xattr_entry_t* restrict out, size_t index)
{
    mutex_acquire(fs_lock(node));
    int retval = ext2_xattr_iterate(node, out, index);
    mutex_release(fs_lock(node));
    return retval;
}

static int ext2_vnode_remove_xattr(vnode_t* restrict node, unsigned space, const char* restrict name)
{
    mutex_acquire(fs_lock(node));

    int retval = ext2_xattr_remove(node, space, name);

    if(retval == 0) {
        touch_locked(node, false);
        retval = write_inode_locked(node);
    }

    mutex_release(fs_lock(node));

    return retval;
}

const vnode_ops_t ext2_vnode_ops = {
    .release = &ext2_vnode_release,
    .lookup = &ext2_vnode_lookup,
    .iterate = &ext2_vnode_iterate,
    .create = &ext2_vnode_create,
    .unlink = &ext2_vnode_unlink,
    .rename = &ext2_vnode_rename,
    .link = &ext2_vnode_link,
    .symlink = &ext2_vnode_symlink,
    .read = &ext2_vnode_read,
    .write = &ext2_vnode_write,
    .readlink = &ext2_vnode_readlink,
    .stat = &ext2_vnode_stat,
    .set_attr = &ext2_vnode_set_attr,
    .fsync = &ext2_vnode_fsync,
    .fault = &ext2_vnode_fault,
    .get_xattr = &ext2_vnode_get_xattr,
    .set_xattr = &ext2_vnode_set_xattr,
    .iterate_xattr = &ext2_vnode_iterate_xattr,
    .remove_xattr = &ext2_vnode_remove_xattr,
};
