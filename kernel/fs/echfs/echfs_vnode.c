#include <vex/block_interface.h>
#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/kprintf.h>
#include <vex/pslab.h>
#include <vex/slab.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/vcache.h>
#include <vex/vnode.h>
#include <vex/wallclock.h>

#include "echfs_internal.h"

static_assert(sizeof(echfs_vnode_context_t) <= SLAB_MAX_SIZE);

static void free_chain(echfs_vfs_context_t* restrict fs, uint64_t block)
{
    while(block != ECHFS_TABLE_EOC && block != ECHFS_TABLE_FREE && block < ECHFS_TABLE_RESERVED) {
        uint64_t next = echfs_block_entry(fs, block);
        echfs_block_free(fs, block);
        block = next;
    }
}

static uint64_t entry_seconds(const timespec_t* restrict time)
{
    if(time->tv_sec < 0) {
        return 0;
    }

    return (uint64_t)(time->tv_sec);
}

static int update_entry_locked(vnode_t* restrict node, const timespec_t* restrict atime, const timespec_t* restrict mtime,
    const timespec_t* restrict ctime)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    // The root has no entry, and the entry of
    // an unlinked file may be another one's now
    if(ctx->dir_entry == UINT64_MAX || ctx->unlinked) {
        return 0;
    }

    echfs_dirent_t entry;
    int retval = echfs_dirent_read(fs, ctx->dir_entry, &entry);

    if(retval < 0) {
        return retval;
    }

    echfs_dirent_t updated = entry;

    if(node->type == VNODE_FILE) {
        updated.file_size = ctx->size;
    }

    if(atime) {
        updated.access_time = entry_seconds(atime);
    }

    if(mtime) {
        updated.modify_time = entry_seconds(mtime);
    }

    if(ctime) {
        updated.change_time = entry_seconds(ctime);
    }

    if(0 == memcmp(&updated, &entry, sizeof(entry))) {
        return 0;
    }

    return echfs_dirent_write(fs, ctx->dir_entry, &updated);
}

static void touch_dir_locked(vnode_t* restrict dir, const timespec_t* restrict now)
{
    int retval = update_entry_locked(dir, nullptr, now, now);

    if(retval < 0) {
        pr_error("echfs: updating a directory's times failed (%d)", retval);
    }
}

static void echfs_vnode_release(vnode_t* restrict node)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    // Held open past unlink, the blocks stay until the last user is gone
    if(ctx->unlinked && node->type != VNODE_DIRECTORY) {
        mutex_acquire(&ctx->fs_ctx->lock);
        free_chain(ctx->fs_ctx, ctx->start_block);
        mutex_release(&ctx->fs_ctx->lock);
    }

    slab_free(ctx);
}

static int lookup_locked(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    uint64_t index;
    int retval = echfs_dirent_find(ctx->fs_ctx, ctx->start_block, name, &index);

    if(retval < 0) {
        return retval;
    }

    *out = index;

    return 0;
}

static int find_directory_entry(const echfs_vfs_context_t* restrict fs, uint64_t directory_id, uint64_t* restrict out)
{
    echfs_dirent_t entry;

    for(uint64_t i = 0; i < fs->dir_entries; ++i) {
        int retval = echfs_dirent_read(fs, i, &entry);

        if(retval < 0) {
            return retval;
        }

        if(entry.directory_id == ECHFS_DIR_ID_END) {
            break;
        }

        if(entry.directory_id == ECHFS_DIR_ID_DELETED) {
            continue;
        }

        if(entry.object_type == 1 && entry.start_block == directory_id) {
            *out = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int parent_nodenumber(const vnode_t* restrict node, ino_t* restrict out)
{
    const echfs_vnode_context_t* ctx = node->ctxdata;

    if(ctx->dir_entry == UINT64_MAX) {
        *out = ECHFS_ROOT_NODENUMBER;
        return 0;
    }

    // The entry of an unlinked directory may be another one's now
    if(ctx->unlinked) {
        *out = node->ino;
        return 0;
    }

    echfs_dirent_t entry;
    int retval = echfs_dirent_read(ctx->fs_ctx, ctx->dir_entry, &entry);

    if(retval < 0) {
        return retval;
    }

    if(entry.directory_id == ECHFS_DIR_ID_ROOT) {
        *out = ECHFS_ROOT_NODENUMBER;
        return 0;
    }

    uint64_t index;
    retval = find_directory_entry(ctx->fs_ctx, entry.directory_id, &index);

    if(retval < 0) {
        return retval;
    }

    *out = index;

    return 0;
}

static int iterate_locked(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    if(dir_context->position < DIR_DOTS_END) {
        ino_t parent;
        int retval = parent_nodenumber(node, &parent);

        if(retval < 0) {
            return retval;
        }

        if(!dir_emit_dots(dir_context, node->ino, parent)) {
            return 0;
        }
    }

    echfs_dirent_t entry;
    dirent_t record;

    for(uint64_t i = dir_context->position - DIR_DOTS_END; i < fs->dir_entries; ++i) {
        int retval = echfs_dirent_read(fs, i, &entry);

        if(retval < 0) {
            return retval;
        }

        if(entry.directory_id == ECHFS_DIR_ID_END) {
            break;
        }

        if(entry.directory_id == ECHFS_DIR_ID_DELETED) {
            continue;
        }

        if(entry.directory_id != ctx->start_block) {
            continue;
        }

        record.d_ino = i;

        if(entry.object_type == 1) {
            record.d_type = DT_DIR;
        }
        else {
            record.d_type = DT_REG;
        }

        memset(record.d_name, 0, NAME_SIZE);
        kstrncpy(record.d_name, entry.name, sizeof(entry.name));

        if(!dir_emit(dir_context, &record, i + 1 + DIR_DOTS_END)) {
            break;
        }
    }

    return 0;
}

static uint64_t alloc_dir_id(const echfs_vfs_context_t* restrict context)
{
    uint64_t max_id = 0;
    echfs_dirent_t entry;

    for(uint64_t i = 0; i < context->dir_entries; ++i) {
        if(echfs_dirent_read(context, i, &entry) < 0 || entry.directory_id == ECHFS_DIR_ID_END) {
            break;
        }

        if(entry.directory_id == ECHFS_DIR_ID_DELETED) {
            continue;
        }

        if(entry.object_type == 1 && entry.start_block > max_id && entry.start_block < ECHFS_DIR_ID_DELETED) {
            max_id = entry.start_block;
        }
    }

    return max_id + 1;
}

static int create_locked(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    if(strnlen(name, NAME_SIZE) >= ECHFS_NAME_SIZE) {
        return -ENAMETOOLONG;
    }

    ino_t existing;

    if(0 == lookup_locked(node, name, &existing)) {
        return -EEXIST;
    }

    uint64_t free_index;

    int retval = echfs_dirent_alloc(fs, &free_index);

    if(retval < 0) {
        return retval;
    }

    echfs_dirent_t entry;
    memset(&entry, 0, sizeof(entry));

    entry.directory_id = ctx->start_block;

    if(type == VNODE_DIRECTORY) {
        entry.object_type = 1;
    }
    else {
        entry.object_type = 0;
    }

    kstrncpy(entry.name, name, sizeof(entry.name));

    entry.permissions = mode;
    entry.owner_uid = credentials->euid;
    entry.owner_gid = credentials->egid;

    timespec_t now;
    wallclock_get(&now);

    entry.access_time = entry_seconds(&now);
    entry.modify_time = entry.access_time;
    entry.change_time = entry.access_time;

    uint64_t start_block;

    if(type == VNODE_DIRECTORY) {
        start_block = alloc_dir_id(fs);
        entry.start_block = start_block;
        entry.file_size = 0;
    }
    else {
        start_block = ECHFS_TABLE_EOC;
        entry.start_block = ECHFS_TABLE_EOC;
        entry.file_size = 0;
    }

    retval = echfs_dirent_write(fs, free_index, &entry);

    if(retval < 0) {
        return retval;
    }

    echfs_vnode_context_t* child_ctx = slab_alloc(sizeof(echfs_vnode_context_t));

    if(child_ctx == nullptr) {
        return -ENOMEM;
    }

    child_ctx->fs_ctx = fs;
    child_ctx->dir_entry = free_index;
    child_ctx->start_block = start_block;
    child_ctx->size = 0;
    child_ctx->cached_block = start_block;
    child_ctx->cached_block_idx = 0;

    vnode_t* child = vnode_alloc(type, &echfs_vnode_ops, child_ctx);

    if(child == nullptr) {
        slab_free(child_ctx);
        return -ENOMEM;
    }

    child->fs = node->fs;
    child->ino = free_index;

    *out = child;

    touch_dir_locked(node, &now);

    return 0;
}

static int check_empty(const echfs_vfs_context_t* restrict fs, uint64_t dir_id)
{
    echfs_dirent_t entry;

    for(uint64_t i = 0; i < fs->dir_entries; ++i) {
        int retval = echfs_dirent_read(fs, i, &entry);

        if(retval < 0) {
            return retval;
        }

        if(entry.directory_id == ECHFS_DIR_ID_END) {
            break;
        }

        if(entry.directory_id == dir_id) {
            return -ENOTEMPTY;
        }
    }

    return 0;
}

static int unlink_locked(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vnode_context_t* target_ctx = target->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    uint64_t index;
    int retval = echfs_dirent_find(fs, ctx->start_block, name, &index);

    if(retval < 0) {
        return retval;
    }

    // The name was looked up without the lock
    // held, so it may have been replaced since
    if(index != target_ctx->dir_entry) {
        return -ENOENT;
    }

    echfs_dirent_t entry;
    retval = echfs_dirent_read(fs, index, &entry);

    if(retval < 0) {
        return retval;
    }

    if(entry.object_type == 1) {
        retval = check_empty(fs, entry.start_block);

        if(retval < 0) {
            return retval;
        }
    }

    echfs_dirent_t deleted;
    memset(&deleted, 0, sizeof(deleted));
    deleted.directory_id = ECHFS_DIR_ID_DELETED;

    retval = echfs_dirent_write(fs, index, &deleted);

    if(retval < 0) {
        return retval;
    }

    target_ctx->unlinked = true;
    vcache_remove(target);

    timespec_t now;
    wallclock_get(&now);
    touch_dir_locked(node, &now);

    return 0;
}

static int rename_locked(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim)
{
    echfs_vnode_context_t* old_ctx = olddir->ctxdata;
    echfs_vnode_context_t* new_ctx = newdir->ctxdata;
    echfs_vnode_context_t* source_ctx = source->ctxdata;
    echfs_vfs_context_t* fs = old_ctx->fs_ctx;

    echfs_dirent_t entry;

    if(strnlen(newname, NAME_SIZE) >= sizeof(entry.name)) {
        return -ENAMETOOLONG;
    }

    uint64_t old_index;
    int retval = echfs_dirent_find(fs, old_ctx->start_block, oldname, &old_index);

    if(retval < 0) {
        return retval;
    }

    // Both names were looked up without the lock
    // held, so either may have been replaced since
    if(old_index != source_ctx->dir_entry) {
        return -ENOENT;
    }

    uint64_t victim_index;
    retval = echfs_dirent_find(fs, new_ctx->start_block, newname, &victim_index);

    if(retval < 0 && retval != -ENOENT) {
        return retval;
    }

    if(victim == nullptr && retval == 0) {
        return -EEXIST;
    }

    if(victim) {
        echfs_vnode_context_t* victim_ctx = victim->ctxdata;

        if(retval == -ENOENT || victim_index != victim_ctx->dir_entry) {
            return -ENOENT;
        }

        if(victim->type == VNODE_DIRECTORY) {
            retval = check_empty(fs, victim_ctx->start_block);

            if(retval < 0) {
                return retval;
            }
        }
    }

    retval = echfs_dirent_read(fs, old_index, &entry);

    if(retval < 0) {
        return retval;
    }

    // Entries are found by their parent's ID, so the
    // entry moves in place and keeps its node number
    timespec_t now;
    wallclock_get(&now);

    entry.directory_id = new_ctx->start_block;
    entry.change_time = entry_seconds(&now);
    memset(entry.name, 0, sizeof(entry.name));
    kstrncpy(entry.name, newname, sizeof(entry.name));

    if(victim) {
        echfs_dirent_t deleted;
        memset(&deleted, 0, sizeof(deleted));
        deleted.directory_id = ECHFS_DIR_ID_DELETED;

        retval = echfs_dirent_write(fs, victim_index, &deleted);

        if(retval < 0) {
            return retval;
        }

        echfs_vnode_context_t* victim_ctx = victim->ctxdata;
        victim_ctx->unlinked = true;
        vcache_remove(victim);
    }

    retval = echfs_dirent_write(fs, old_index, &entry);

    if(retval < 0) {
        return retval;
    }

    touch_dir_locked(olddir, &now);

    if(newdir != olddir) {
        touch_dir_locked(newdir, &now);
    }

    return 0;
}

static int chain_block(vnode_t* restrict node, uint64_t block_index, bool create, uintptr_t* restrict out)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    if(ctx->start_block == ECHFS_TABLE_EOC) {
        if(!create) {
            return -EIO;
        }

        uintptr_t new_block = echfs_block_alloc(fs);

        if(new_block == UINTPTR_MAX) {
            return -ENOSPC;
        }

        // An unlinked file's entry may
        // belong to another file by now
        if(!ctx->unlinked) {
            echfs_dirent_t entry;
            int retval = echfs_dirent_read(fs, ctx->dir_entry, &entry);

            if(retval < 0) {
                echfs_block_free(fs, new_block);
                return retval;
            }

            entry.start_block = new_block;
            echfs_dirent_write(fs, ctx->dir_entry, &entry);
        }

        ctx->start_block = new_block;
        ctx->cached_block = new_block;
        ctx->cached_block_idx = 0;
    }

    if(ctx->start_block >= fs->total_blocks) {
        return -EIO;
    }

    uintptr_t block = ctx->start_block;
    uint64_t index = 0;

    if(block_index >= ctx->cached_block_idx && ctx->cached_block < fs->total_blocks) {
        block = ctx->cached_block;
        index = ctx->cached_block_idx;
    }

    while(index < block_index) {
        uintptr_t next = echfs_block_next(fs, block);

        if(next == UINTPTR_MAX) {
            if(!create) {
                return -EIO;
            }

            next = echfs_block_alloc(fs);

            if(next == UINTPTR_MAX) {
                return -ENOSPC;
            }

            echfs_block_link(fs, block, next);
        }

        block = next;
        index += 1;
    }

    ctx->cached_block = block;
    ctx->cached_block_idx = block_index;

    *out = block;

    return 0;
}

static int echfs_bmap(vnode_t* restrict node, uint64_t sector, bool create, fcache_extent_t* restrict out)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    uint64_t in_block = sector % fs->sectors_per_block;
    uintptr_t block;
    int retval = chain_block(node, sector / fs->sectors_per_block, create, &block);

    if(retval < 0) {
        return retval;
    }

    out->lba = echfs_block_lba(fs, block) + in_block;
    out->count = fs->sectors_per_block - in_block;
    out->mapped = true;

    return 0;
}

static ssize_t read_locked(vnode_t* restrict node, void* restrict buffer, size_t size, off_t offset)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    return fcache_read(node, &echfs_bmap, buffer, size, offset, ctx->size);
}

static ssize_t write_span_locked(vnode_t* restrict node, const void* restrict buffer, size_t size, off_t offset)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    ssize_t written = fcache_write(node, &echfs_bmap, buffer, size, offset, ctx->size);

    if(written <= 0) {
        return written;
    }

    if((uint64_t)(offset + written) > ctx->size) {
        ctx->size = offset + written;
    }

    timespec_t now;
    wallclock_get(&now);
    update_entry_locked(node, nullptr, &now, &now);

    return written;
}

static int zero_fill_locked(vnode_t* restrict node, off_t from, off_t to)
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

// Blocks come off the allocator as they were left on disk,
// so a gap past the end of the file is zeroed out explicitly
static ssize_t write_locked(vnode_t* restrict node, const void* restrict buffer, size_t size, off_t offset)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    if((uint64_t)(offset) > ctx->size) {
        int retval = zero_fill_locked(node, ctx->size, offset);

        if(retval < 0) {
            return retval;
        }
    }

    return write_span_locked(node, buffer, size, offset);
}

static int shrink_locked(vnode_t* restrict node, uint64_t size)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    // Dirty pages past the new end must not
    // reach blocks that are freed below
    fcache_truncate(node, size);

    uint64_t keep = (size + fs->block_size - 1) / fs->block_size;

    if(keep == 0) {
        free_chain(fs, ctx->start_block);
        ctx->start_block = ECHFS_TABLE_EOC;
    }
    else {
        uintptr_t last = ctx->start_block;

        for(uint64_t i = 1; i < keep; ++i) {
            last = echfs_block_next(fs, last);

            if(last == UINTPTR_MAX) {
                return -EIO;
            }
        }

        uintptr_t rest = echfs_block_next(fs, last);

        if(rest != UINTPTR_MAX) {
            echfs_block_link(fs, last, ECHFS_TABLE_EOC);
            free_chain(fs, rest);
        }
    }

    ctx->size = size;
    ctx->cached_block = ctx->start_block;
    ctx->cached_block_idx = 0;

    if(ctx->unlinked) {
        return 0;
    }

    echfs_dirent_t entry;
    int retval = echfs_dirent_read(fs, ctx->dir_entry, &entry);

    if(retval < 0) {
        return retval;
    }

    entry.file_size = size;
    entry.start_block = ctx->start_block;

    return echfs_dirent_write(fs, ctx->dir_entry, &entry);
}

static int set_owner_locked(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    // The root has no entry to keep them in, and the
    // entry of an unlinked file may be another one's now
    if(ctx->dir_entry == UINT64_MAX) {
        return -EPERM;
    }

    if(ctx->unlinked) {
        return -ENOENT;
    }

    // Owner fields on disk are 16 bits wide
    if(((attr->mask & VATTR_UID) && attr->uid > UINT16_MAX) || ((attr->mask & VATTR_GID) && attr->gid > UINT16_MAX)) {
        return -EINVAL;
    }

    echfs_dirent_t entry;
    int retval = echfs_dirent_read(fs, ctx->dir_entry, &entry);

    if(retval < 0) {
        return retval;
    }

    if(attr->mask & VATTR_MODE) {
        entry.permissions = attr->mode & ~S_IFMT;
    }

    if(attr->mask & VATTR_UID) {
        entry.owner_uid = attr->uid;
    }

    if(attr->mask & VATTR_GID) {
        entry.owner_gid = attr->gid;
    }

    return echfs_dirent_write(fs, ctx->dir_entry, &entry);
}

static int resize_locked(vnode_t* restrict node, off_t size)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    if((uint64_t)(size) > ctx->size) {
        return zero_fill_locked(node, ctx->size, size);
    }

    return shrink_locked(node, size);
}

static int set_attr_locked(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    if(attr->mask & (VATTR_MODE | VATTR_UID | VATTR_GID)) {
        int retval = set_owner_locked(node, attr);

        if(retval < 0) {
            return retval;
        }
    }

    if((attr->mask & VATTR_SIZE) && (uint64_t)(attr->size) != ctx->size) {
        int retval = resize_locked(node, attr->size);

        if(retval < 0) {
            return retval;
        }
    }

    const timespec_t* atime = nullptr;
    const timespec_t* mtime = nullptr;
    const timespec_t* ctime = nullptr;

    if(attr->mask & VATTR_ATIME) {
        atime = &attr->atime;
    }

    if(attr->mask & VATTR_MTIME) {
        mtime = &attr->mtime;
    }

    if(attr->mask & VATTR_CTIME) {
        ctime = &attr->ctime;
    }

    return update_entry_locked(node, atime, mtime, ctime);
}

static int stat_locked(vnode_t* restrict node, stat_t* restrict out)
{
    echfs_vnode_context_t* ctx = node->ctxdata;
    echfs_vfs_context_t* fs = ctx->fs_ctx;

    memset(out, 0, sizeof(stat_t));

    out->st_ino = node->ino;
    out->st_size = ctx->size;
    out->st_blksize = fs->block_size;
    out->st_blocks = (ctx->size + fs->volume->sector_size - 1) / fs->volume->sector_size;

    if(ctx->dir_entry == UINT64_MAX) {
        out->st_mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    }
    else if(!ctx->unlinked) {
        echfs_dirent_t entry;
        int retval = echfs_dirent_read(fs, ctx->dir_entry, &entry);

        if(retval < 0) {
            return retval;
        }

        out->st_uid = entry.owner_uid;
        out->st_gid = entry.owner_gid;
        out->st_mode = entry.permissions;
        out->st_atim.tv_sec = entry.access_time;
        out->st_mtim.tv_sec = entry.modify_time;
        out->st_ctim.tv_sec = entry.change_time;
    }

    if(node->type == VNODE_DIRECTORY) {
        out->st_mode = (out->st_mode & ~S_IFMT) | S_IFDIR;
    }
    else {
        out->st_mode = (out->st_mode & ~S_IFMT) | S_IFREG;
    }

    out->st_nlink = 1;

    return 0;
}

static mutex_t* fs_lock(vnode_t* restrict node)
{
    echfs_vnode_context_t* context = node->ctxdata;
    return &context->fs_ctx->lock;
}

static int echfs_vnode_lookup(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    mutex_acquire(fs_lock(node));
    int retval = lookup_locked(node, name, out);
    mutex_release(fs_lock(node));
    return retval;
}

static int echfs_vnode_iterate(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    mutex_acquire(fs_lock(node));
    int retval = iterate_locked(node, dir_context);
    mutex_release(fs_lock(node));
    return retval;
}

static int echfs_vnode_create(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    mutex_acquire(fs_lock(node));
    int retval = create_locked(node, name, type, mode, credentials, out);
    mutex_release(fs_lock(node));
    return retval;
}

static int echfs_vnode_unlink(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    mutex_acquire(fs_lock(node));
    int retval = unlink_locked(node, name, target);
    mutex_release(fs_lock(node));
    return retval;
}

static int echfs_vnode_rename(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim)
{
    mutex_acquire(fs_lock(olddir));
    int retval = rename_locked(olddir, oldname, source, newdir, newname, victim);
    mutex_release(fs_lock(olddir));
    return retval;
}

static ssize_t echfs_vnode_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    mutex_acquire(fs_lock(node));
    ssize_t retval = read_locked(node, buffer, size, offset);
    mutex_release(fs_lock(node));
    return retval;
}

static ssize_t echfs_vnode_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    mutex_acquire(fs_lock(node));
    ssize_t retval = write_locked(node, buffer, size, offset);
    mutex_release(fs_lock(node));
    return retval;
}

static int echfs_vnode_fault(vnode_t* restrict node, off_t offset, pmap_t* restrict pmap, uintptr_t virt, unsigned vprot)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    mutex_acquire(fs_lock(node));
    int retval = fcache_fault(node, &echfs_bmap, offset, ctx->size, pmap, virt, vprot);
    mutex_release(fs_lock(node));

    return retval;
}

static int echfs_vnode_fsync(vnode_t* restrict node)
{
    echfs_vnode_context_t* ctx = node->ctxdata;

    mutex_acquire(fs_lock(node));
    int retval = fcache_writeback(node, &echfs_bmap, ctx->size);
    mutex_release(fs_lock(node));
    return retval;
}

static int echfs_vnode_stat(vnode_t* restrict node, stat_t* restrict out)
{
    mutex_acquire(fs_lock(node));
    int retval = stat_locked(node, out);
    mutex_release(fs_lock(node));
    return retval;
}

static int echfs_vnode_set_attr(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    mutex_acquire(fs_lock(node));
    int retval = set_attr_locked(node, attr);
    mutex_release(fs_lock(node));
    return retval;
}

const vnode_ops_t echfs_vnode_ops = {
    .release = &echfs_vnode_release,
    .lookup = &echfs_vnode_lookup,
    .iterate = &echfs_vnode_iterate,
    .create = &echfs_vnode_create,
    .unlink = &echfs_vnode_unlink,
    .rename = &echfs_vnode_rename,
    .read = &echfs_vnode_read,
    .write = &echfs_vnode_write,
    .stat = &echfs_vnode_stat,
    .set_attr = &echfs_vnode_set_attr,
    .fsync = &echfs_vnode_fsync,
    .fault = &echfs_vnode_fault,
};
