#include <vex/assert.h>
#include <vex/bitmap.h>
#include <vex/block_interface.h>
#include <vex/ctype.h>
#include <vex/dirent.h>
#include <vex/endian.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/kprintf.h>
#include <vex/mount.h>
#include <vex/pslab.h>
#include <vex/slab.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/vcache.h>
#include <vex/vnode.h>
#include <vex/wallclock.h>

#include "fat_internal.h"

static ino_t fat_nodenumber(const fat_vfs_context_t* restrict fs, uintptr_t dir_cluster, size_t index)
{
    size_t num_entries = fs->sector_size / sizeof(fat_entry_t);
    uintptr_t entry_offset = index % num_entries * sizeof(fat_entry_t);
    uintptr_t lba;

    if(dir_cluster == 0) {
        lba = fs->data_start - fs->rootdir_sectors + index / num_entries;
    }
    else {
        size_t cluster_entries = fs->cluster_size_b / sizeof(fat_entry_t);
        size_t cluster_idx = index / cluster_entries;
        uintptr_t cluster = dir_cluster;

        for(size_t i = 0; i < cluster_idx; ++i) {
            cluster = fat_cluster_next(fs, cluster);

            kassert(cluster < UINTPTR_MAX);
        }

        lba = fat_cluster_lba(fs, cluster) + index % cluster_entries / num_entries;
    }

    ino_t result = 0;
    result |= lba << 32;
    result |= entry_offset;
    return result;
}

static bool is_shortname_char(char c)
{
    static const char ok[] = "$%-_@~`!(){}^#&";

    if(c >= 128) {
        return true;
    }

    if(c >= 'A' && c <= 'Z') {
        return true;
    }

    if(c >= '0' && c <= '9') {
        return true;
    }

    for(const char* it = ok; *it; it += 1) {
        if(c == *it) {
            return true;
        }
    }

    return false;
}

static void shortname_parts(const char* restrict filename, char stem[6], unsigned* restrict stem_len, char ext[3],
    unsigned* restrict ext_len)
{
    *stem_len = 0;
    *ext_len = 0;

    const char* last_dot = nullptr;
    for(const char* it = filename; *it; it += 1) {
        if(*it == '.') {
            last_dot = it;
        }
    }

    const char* stem_end;

    if(last_dot == nullptr) {
        stem_end = filename + strnlen(filename, NAME_SIZE);
    }
    else {
        stem_end = last_dot;
    }

    for(const char* it = filename; it != stem_end && *stem_len < 6; it += 1) {
        char chr = toupper(*it);

        if(chr == ' ' || chr == '.') {
            continue;
        }

        if(is_shortname_char(chr)) {
            stem[*stem_len] = chr;
            *stem_len += 1;
        }
    }

    if(last_dot) {
        for(const char* it = last_dot + 1; *it && *ext_len < 3; it += 1) {
            char chr = toupper(*it);

            if(chr == ' ') {
                continue;
            }

            if(is_shortname_char(chr)) {
                ext[*ext_len] = chr;
                *ext_len += 1;
            }
        }
    }
}

static void shortname_build(const char* restrict name, unsigned tail_n, uint8_t out[11])
{
    memset(out, 0x20, 11);

    char stem[6], ext[3];
    unsigned stem_len, ext_len;
    shortname_parts(name, stem, &stem_len, ext, &ext_len);

    char tail[5];
    unsigned tail_len = 1;
    tail[0] = '~';

    if(tail_n >= 100) {
        tail[tail_len] = '0' + tail_n / 100;
        tail_len += 1;
    }

    if(tail_n >= 10) {
        tail[tail_len] = '0' + (tail_n / 10) % 10;
        tail_len += 1;
    }

    tail[tail_len] = '0' + tail_n % 10;
    tail_len += 1;

    unsigned stem_max = 8 - tail_len;
    if(stem_len > stem_max) {
        stem_len = stem_max;
    }

    unsigned pos = 0;

    for(unsigned i = 0; i < stem_len; ++i) {
        out[pos] = stem[i];
        pos += 1;
    }

    for(unsigned i = 0; i < tail_len; ++i) {
        out[pos] = tail[i];
        pos += 1;
    }

    for(unsigned i = 0; i < ext_len; ++i) {
        out[8 + i] = ext[i];
    }
}

static bool shortname_pick(const fat_vfs_context_t* restrict fs, uintptr_t dir_cluster, const char* restrict name, uint8_t out[11])
{
    bitmap_t taken[32] = {};
    fat_dirent_t taken_dirent;
    size_t taken_offset = 0;

    static_assert(sizeof(taken) * sizeof(bitmap_t) * CHAR_BIT > 999);

    while(0 == fat_dirent_read(fs, dir_cluster, &taken_offset, &taken_dirent)) {
        int tilde = -1;

        for(int i = 0; i < 7; ++i) {
            if(taken_dirent.base.name[i] == '~') {
                tilde = i;
                break;
            }
        }

        if(tilde < 0) {
            continue;
        }

        unsigned n = 0;

        for(int i = tilde + 1; i < 8 && taken_dirent.base.name[i] != ' '; ++i) {
            uint8_t chr = taken_dirent.base.name[i];

            if(chr < '0' || chr > '9') {
                n = 0;
                break;
            }

            n *= 10;
            n += chr - '0';
        }

        if(n >= 1 && n <= 999) {
            bitmap_set(taken, n);
        }
    }

    for(unsigned n = 1; n < 999; ++n) {
        if(!bitmap_isset(taken, n)) {
            shortname_build(name, n, out);
            return true;
        }
    }

    return false;
}

static int find_entry(const fat_vfs_context_t* restrict fs, uintptr_t dir_cluster, const char* restrict name, fat_dirent_t* restrict out,
    size_t* restrict out_offset)
{
    size_t offset = 0;

    for(;;) {
        int retval = fat_dirent_read(fs, dir_cluster, &offset, out);

        if(retval < 0) {
            return retval;
        }

        if(0 == strncasecmp(out->full_name, name, NAME_MAX)) {
            *out_offset = offset - 1;
            return 0;
        }
    }
}

static void free_chain(fat_vfs_context_t* restrict fs, uintptr_t cluster)
{
    while(cluster && cluster != UINTPTR_MAX) {
        uintptr_t next = fat_cluster_next(fs, cluster);
        fat_cluster_free(fs, cluster);
        cluster = next;
    }
}

static bool has_entry(const vnode_t* restrict node)
{
    const fat_vnode_context_t* context = node->ctxdata;

    if(node->type == VNODE_DIRECTORY && context->first_cluster == context->fs_ctx->root_cluster) {
        return false;
    }

    return !context->unlinked;
}

static void stamp_new_entry(fat_dir_entry_t* restrict entry, const timespec_t* restrict now)
{
    fat_timestamp_t stamp;
    fat_time_pack(now, &stamp);

    entry->create_date = stamp.date;
    entry->create_time = stamp.time;
    entry->create_time_tenth = stamp.tenth;
    entry->write_date = stamp.date;
    entry->write_time = stamp.time;
    entry->access_date = stamp.date;
}

static int update_entry_locked(vnode_t* restrict node, const timespec_t* restrict atime, const timespec_t* restrict mtime)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    if(!has_entry(node)) {
        return 0;
    }

    fat_dirent_t dirent;
    size_t dir_off = context->dir_offset;
    int retval = fat_dirent_read(fs, context->dir_cluster, &dir_off, &dirent);

    if(retval < 0) {
        return retval;
    }

    fat_dir_entry_t entry = dirent.base;
    fat_timestamp_t stamp;

    if(node->type == VNODE_FILE) {
        entry.file_size = htole32(context->size_bytes);
    }

    if(atime) {
        fat_time_pack(atime, &stamp);
        entry.access_date = stamp.date;
    }

    if(mtime) {
        fat_time_pack(mtime, &stamp);
        entry.write_date = stamp.date;
        entry.write_time = stamp.time;
    }

    if(0 == memcmp(&entry, &dirent.base, sizeof(entry))) {
        return 0;
    }

    return fat_dirent_update(fs, context->dir_cluster, context->dir_offset, &entry);
}

static void touch_dir_locked(vnode_t* restrict dir, const timespec_t* restrict now)
{
    int retval = update_entry_locked(dir, nullptr, now);

    if(retval < 0) {
        pr_error("fat: updating a directory's times failed: %s", strerror(retval));
    }
}

static void fat_vnode_release(vnode_t* restrict node)
{
    fat_vnode_context_t* context = node->ctxdata;

    // Held open past unlink, the clusters
    // stay until the last user is gone
    if(context->unlinked) {
        mutex_acquire(&context->fs_ctx->lock);
        free_chain(context->fs_ctx, context->first_cluster);
        mutex_release(&context->fs_ctx->lock);
    }

    slab_free(context);
}

static int lookup_locked(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_dirent_t dirent;
    size_t offset;

    int retval = find_entry(context->fs_ctx, context->first_cluster, name, &dirent, &offset);

    if(retval < 0) {
        return retval;
    }

    *out = fat_nodenumber(context->fs_ctx, context->first_cluster, offset);

    return 0;
}

static bool is_dot_name(const char* restrict name)
{
    return 0 == strcmp(name, ".") || 0 == strcmp(name, "..");
}

static int read_parent_cluster(const fat_vfs_context_t* restrict fs, uintptr_t dir_cluster, uintptr_t* restrict out)
{
    fat_dirent_t dotdot;
    size_t offset = 1;
    int retval = fat_dirent_read(fs, dir_cluster, &offset, &dotdot);

    if(retval < 0) {
        return retval;
    }

    if(offset != 2 || strcmp(dotdot.full_name, "..")) {
        return -EIO;
    }

    uintptr_t parent_cluster = fat_dirent_cluster(&dotdot.base);

    // A ".." pointing at the root holds 0 on every
    // FAT type, even though the FAT32 root has a real cluster
    if(parent_cluster == 0) {
        parent_cluster = fs->root_cluster;
    }

    *out = parent_cluster;

    return 0;
}

// A directory's node number comes from its entry in its own parent,
// so the parent's number needs the entry in the grandparent
static int parent_nodenumber(const fat_vfs_context_t* restrict fs, uintptr_t dir_cluster, ino_t* restrict out)
{
    if(dir_cluster == fs->root_cluster) {
        *out = FAT_ROOT_NODENUMBER;
        return 0;
    }

    uintptr_t parent_cluster;
    int retval = read_parent_cluster(fs, dir_cluster, &parent_cluster);

    if(retval < 0) {
        return retval;
    }

    if(parent_cluster == fs->root_cluster) {
        *out = FAT_ROOT_NODENUMBER;
        return 0;
    }

    uintptr_t grandparent_cluster;
    retval = read_parent_cluster(fs, parent_cluster, &grandparent_cluster);

    if(retval < 0) {
        return retval;
    }

    fat_dirent_t entry;
    size_t offset = 0;

    for(;;) {
        retval = fat_dirent_read(fs, grandparent_cluster, &offset, &entry);

        if(retval < 0) {
            return retval;
        }

        if(0 == (entry.base.attribute & FAT_ATTR_DIRECTORY) || is_dot_name(entry.full_name)) {
            continue;
        }

        if(fat_dirent_cluster(&entry.base) == parent_cluster) {
            *out = fat_nodenumber(fs, grandparent_cluster, offset - 1);
            return 0;
        }
    }
}

static int iterate_locked(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    if(dir_context->position < DIR_DOTS_END) {
        ino_t parent;
        int retval = parent_nodenumber(fs, context->first_cluster, &parent);

        if(retval < 0) {
            return retval;
        }

        if(!dir_emit_dots(dir_context, node->ino, parent)) {
            return 0;
        }
    }

    fat_dirent_t entry;
    dirent_t record;
    size_t offset = dir_context->position - DIR_DOTS_END;

    for(;;) {
        int retval = fat_dirent_read(fs, context->first_cluster, &offset, &entry);

        if(retval == -ENOENT) {
            return 0;
        }

        if(retval < 0) {
            return retval;
        }

        // The root has no dot entries on the disk, and the node numbers of the
        // others differ from what stat gives, so dir_emit_dots stands in for all of them
        if((entry.base.attribute & FAT_ATTR_VOLUME_ID) || is_dot_name(entry.full_name)) {
            continue;
        }

        record.d_ino = fat_nodenumber(fs, context->first_cluster, offset - 1);

        if(entry.base.attribute & FAT_ATTR_DIRECTORY) {
            record.d_type = DT_DIR;
        }
        else {
            record.d_type = DT_REG;
        }

        memset(record.d_name, 0, NAME_SIZE);
        kstrncpy(record.d_name, entry.full_name, NAME_SIZE);

        if(!dir_emit(dir_context, &record, offset + DIR_DOTS_END)) {
            return 0;
        }
    }
}

static int create_locked(vnode_t* restrict node, const char* restrict name, unsigned type, vnode_t** restrict out)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    if(fs->readonly) {
        return -EROFS;
    }

    ino_t existing_nodenumber;

    if(0 == lookup_locked(node, name, &existing_nodenumber)) {
        return -EEXIST;
    }

    size_t name_len = strnlen(name, NAME_SIZE);

    unsigned num_lfn = name_len + 12;
    num_lfn /= 13;

    unsigned slots = num_lfn + 1;

    size_t slot_offset;
    int retval = fat_dirent_alloc(fs, context->first_cluster, slots, &slot_offset);

    if(retval < 0) {
        return retval;
    }

    uintptr_t first_cluster = 0;

    if(type == VNODE_DIRECTORY) {
        first_cluster = fat_cluster_alloc_dir(fs);

        if(first_cluster == UINTPTR_MAX) {
            return -ENOSPC;
        }
    }

    fat_dirent_t dirent;
    memset(&dirent, 0, sizeof(fat_dirent_t));

    if(!shortname_pick(fs, context->first_cluster, name, dirent.base.name)) {
        if(first_cluster)
            fat_cluster_free(fs, first_cluster);
        return -ENOSPC;
    }

    kstrncpy(dirent.full_name, name, NAME_SIZE);

    if(type == VNODE_DIRECTORY) {
        dirent.base.attribute = FAT_ATTR_DIRECTORY;
    }
    else {
        dirent.base.attribute = FAT_ATTR_ARCHIVE;
    }

    dirent.base.first_cluster_hi = htole16(first_cluster >> 16);
    dirent.base.first_cluster_lo = htole16(first_cluster & 0xFFFF);
    dirent.base.file_size = 0;

    timespec_t now;
    wallclock_get(&now);
    stamp_new_entry(&dirent.base, &now);

    retval = fat_dirent_write(fs, context->first_cluster, slot_offset + num_lfn, &dirent);

    if(retval < 0) {
        if(first_cluster)
            fat_cluster_free(fs, first_cluster);
        return retval;
    }

    if(type == VNODE_DIRECTORY) {
        fat_entry_t dot_entry;
        memset(&dot_entry, 0, sizeof(fat_entry_t));

        dot_entry.dir.attribute = FAT_ATTR_DIRECTORY;
        stamp_new_entry(&dot_entry.dir, &now);
        dot_entry.dir.first_cluster_hi = htole16(first_cluster >> 16);
        dot_entry.dir.first_cluster_lo = htole16(first_cluster & 0xFFFF);

        memset(dot_entry.dir.name, 0x20, 11);
        dot_entry.dir.name[0] = '.';

        retval = fat_dirent_update(fs, first_cluster, 0, &dot_entry.dir);

        if(retval < 0) {
            fat_cluster_free(fs, first_cluster);
            return retval;
        }

        memset(dot_entry.dir.name, 0x20, 11);
        dot_entry.dir.name[0] = '.';
        dot_entry.dir.name[1] = '.';

        // A ".." pointing at the root holds 0 on every
        // FAT type, even though the FAT32 root has a real cluster
        uintptr_t parent_cluster = context->first_cluster;

        if(parent_cluster == fs->root_cluster) {
            parent_cluster = 0;
        }
        dot_entry.dir.first_cluster_hi = htole16(parent_cluster >> 16);
        dot_entry.dir.first_cluster_lo = htole16(parent_cluster & 0xFFFF);

        retval = fat_dirent_update(fs, first_cluster, 1, &dot_entry.dir);

        if(retval < 0) {
            fat_cluster_free(fs, first_cluster);
            return retval;
        }
    }

    fat_vnode_context_t* ctx = slab_alloc(sizeof(fat_vnode_context_t));

    if(ctx == nullptr) {
        fat_cluster_free(fs, first_cluster);
        return -ENOMEM;
    }

    ctx->fs_ctx = fs;
    ctx->first_cluster = first_cluster;
    ctx->size_bytes = 0;
    ctx->dir_cluster = context->first_cluster;
    ctx->dir_offset = slot_offset + num_lfn;
    ctx->cached_cluster = first_cluster;
    ctx->cached_cluster_idx = 0;

    vnode_t* vnode = vnode_alloc(type, &fat_vnode_ops, ctx);

    if(vnode == nullptr) {
        slab_free(ctx);
        fat_cluster_free(fs, first_cluster);
        return -ENOMEM;
    }

    vnode->fs = node->fs;
    vnode->ino = fat_nodenumber(fs, context->first_cluster, slot_offset + num_lfn);

    *out = vnode;

    touch_dir_locked(node, &now);

    return 0;
}

static int check_empty(const fat_vfs_context_t* restrict fs, uintptr_t cluster)
{
    fat_dirent_t dirent;
    size_t offset = 0;

    for(;;) {
        int retval = fat_dirent_read(fs, cluster, &offset, &dirent);

        if(retval == -ENOENT) {
            return 0;
        }

        if(retval < 0) {
            return retval;
        }

        if(is_dot_name(dirent.full_name)) {
            continue;
        }

        if(0 == (dirent.base.attribute & FAT_ATTR_VOLUME_ID)) {
            return -ENOTEMPTY;
        }
    }
}

static int unlink_locked(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vnode_context_t* target_context = target->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    if(fs->readonly) {
        return -EROFS;
    }

    if(target->type == VNODE_DIRECTORY) {
        int retval = check_empty(fs, target_context->first_cluster);

        if(retval < 0) {
            return retval;
        }
    }

    fat_dirent_t dirent;
    size_t entry_offset;
    int retval = find_entry(fs, context->first_cluster, name, &dirent, &entry_offset);

    if(retval < 0) {
        return retval;
    }

    // The name was looked up without the lock
    // held, so it may have been replaced since
    if(fat_nodenumber(fs, context->first_cluster, entry_offset) != target->ino) {
        return -ENOENT;
    }

    retval = fat_dirent_remove(fs, context->first_cluster, entry_offset, dirent.num_lfn);

    if(retval < 0) {
        return retval;
    }

    target_context->unlinked = true;
    vcache_remove(target);

    timespec_t now;
    wallclock_get(&now);
    touch_dir_locked(node, &now);

    return 0;
}

static int set_dotdot(fat_vfs_context_t* restrict fs, uintptr_t dir_cluster, uintptr_t parent_cluster)
{
    fat_dirent_t dotdot;
    size_t offset = 1;
    int retval = fat_dirent_read(fs, dir_cluster, &offset, &dotdot);

    if(retval < 0) {
        return retval;
    }

    if(offset != 2 || strcmp(dotdot.full_name, "..")) {
        return -EIO;
    }

    // A ".." pointing at the root holds 0 on every
    // FAT type, even though the FAT32 root has a real cluster
    if(parent_cluster == fs->root_cluster) {
        parent_cluster = 0;
    }

    dotdot.base.first_cluster_hi = htole16(parent_cluster >> 16);
    dotdot.base.first_cluster_lo = htole16(parent_cluster & 0xFFFF);

    return fat_dirent_update(fs, dir_cluster, 1, &dotdot.base);
}

static int rename_locked(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim)
{
    fat_vnode_context_t* old_context = olddir->ctxdata;
    fat_vnode_context_t* new_context = newdir->ctxdata;
    fat_vnode_context_t* source_context = source->ctxdata;
    fat_vfs_context_t* fs = old_context->fs_ctx;

    if(fs->readonly) {
        return -EROFS;
    }

    fat_dirent_t old_entry;
    size_t old_offset;
    int retval = find_entry(fs, old_context->first_cluster, oldname, &old_entry, &old_offset);

    if(retval < 0) {
        return retval;
    }

    // Both names were looked up without the lock
    // held, so either may have been replaced since
    if(fat_nodenumber(fs, old_context->first_cluster, old_offset) != source->ino) {
        return -ENOENT;
    }

    fat_dirent_t victim_entry;
    size_t victim_offset;
    retval = find_entry(fs, new_context->first_cluster, newname, &victim_entry, &victim_offset);

    if(retval < 0 && retval != -ENOENT) {
        return retval;
    }

    if(victim == nullptr && retval == 0) {
        return -EEXIST;
    }

    if(victim) {
        if(retval == -ENOENT || fat_nodenumber(fs, new_context->first_cluster, victim_offset) != victim->ino) {
            return -ENOENT;
        }

        if(victim->type == VNODE_DIRECTORY) {
            fat_vnode_context_t* victim_context = victim->ctxdata;
            retval = check_empty(fs, victim_context->first_cluster);

            if(retval < 0) {
                return retval;
            }
        }
    }

    fat_dirent_t entry;
    memset(&entry, 0, sizeof(fat_dirent_t));
    memcpy(&entry.base, &old_entry.base, sizeof(fat_dir_entry_t));
    kstrncpy(entry.full_name, newname, NAME_SIZE);

    if(!shortname_pick(fs, new_context->first_cluster, newname, entry.base.name)) {
        return -ENOSPC;
    }

    unsigned num_lfn = strnlen(newname, NAME_SIZE) + 12;
    num_lfn /= 13;

    size_t slot_offset;
    retval = fat_dirent_alloc(fs, new_context->first_cluster, num_lfn + 1, &slot_offset);

    if(retval < 0) {
        return retval;
    }

    size_t new_offset = slot_offset + num_lfn;
    retval = fat_dirent_write(fs, new_context->first_cluster, new_offset, &entry);

    if(retval < 0) {
        return retval;
    }

    if(victim) {
        retval = fat_dirent_remove(fs, new_context->first_cluster, victim_offset, victim_entry.num_lfn);

        if(retval < 0) {
            pr_error("fat: rename: removing the replaced entry failed (%d)", retval);
        }
    }

    retval = fat_dirent_remove(fs, old_context->first_cluster, old_offset, old_entry.num_lfn);

    if(retval < 0) {
        pr_error("fat: rename: removing the old entry failed (%d)", retval);
    }

    if(source->type == VNODE_DIRECTORY && olddir != newdir) {
        retval = set_dotdot(fs, source_context->first_cluster, new_context->first_cluster);

        if(retval < 0) {
            pr_error("fat: rename: updating \"..\" failed (%d)", retval);
        }
    }

    if(victim) {
        fat_vnode_context_t* victim_context = victim->ctxdata;
        victim_context->unlinked = true;
        vcache_remove(victim);
    }

    source_context->dir_cluster = new_context->first_cluster;
    source_context->dir_offset = new_offset;

    vcache_rekey(source, fat_nodenumber(fs, new_context->first_cluster, new_offset));

    timespec_t now;
    wallclock_get(&now);
    touch_dir_locked(olddir, &now);

    if(newdir != olddir) {
        touch_dir_locked(newdir, &now);
    }

    return 0;
}

static int chain_cluster(vnode_t* restrict node, size_t cluster_index, bool create, uintptr_t* restrict out)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    if(context->first_cluster == 0) {
        if(!create) {
            return -EIO;
        }

        uintptr_t new_cluster = fat_cluster_alloc_zeroed(fs);

        if(new_cluster == UINTPTR_MAX) {
            return -ENOSPC;
        }

        // An unlinked file's entry slot
        // may belong to another file by now
        if(!context->unlinked) {
            fat_dirent_t dirent;
            size_t dir_off = context->dir_offset;
            int retval = fat_dirent_read(fs, context->dir_cluster, &dir_off, &dirent);

            if(retval < 0) {
                fat_cluster_free(fs, new_cluster);
                return retval;
            }

            dirent.base.first_cluster_hi = htole16(new_cluster >> 16);
            dirent.base.first_cluster_lo = htole16(new_cluster & 0xFFFF);
            fat_dirent_update(fs, context->dir_cluster, context->dir_offset, &dirent.base);
        }

        context->first_cluster = new_cluster;
        context->cached_cluster = new_cluster;
        context->cached_cluster_idx = 0;
    }

    if(!fat_cluster_valid(fs, context->first_cluster)) {
        return -EIO;
    }

    uintptr_t cluster = context->first_cluster;
    size_t index = 0;

    if(cluster_index >= context->cached_cluster_idx && fat_cluster_valid(fs, context->cached_cluster)) {
        cluster = context->cached_cluster;
        index = context->cached_cluster_idx;
    }

    while(index < cluster_index) {
        uintptr_t next = fat_cluster_next(fs, cluster);

        if(next == UINTPTR_MAX) {
            if(!create) {
                return -EIO;
            }

            next = fat_cluster_alloc_zeroed(fs);

            if(next == UINTPTR_MAX) {
                return -ENOSPC;
            }

            fat_cluster_link(fs, cluster, next);
        }

        cluster = next;
        index += 1;
    }

    context->cached_cluster = cluster;
    context->cached_cluster_idx = cluster_index;

    *out = cluster;

    return 0;
}

static int fat_bmap(vnode_t* restrict node, uint64_t sector, bool create, fcache_extent_t* restrict out)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    if(fs->sector_size != fs->volume->sector_size) {
        return -EIO;
    }

    size_t in_cluster = sector % fs->cluster_size_s;
    uintptr_t cluster;
    int retval = chain_cluster(node, sector / fs->cluster_size_s, create, &cluster);

    if(retval < 0) {
        return retval;
    }

    out->lba = fat_cluster_lba(fs, cluster) + in_cluster;
    out->count = fs->cluster_size_s - in_cluster;
    out->mapped = true;

    return 0;
}

static ssize_t read_locked(vnode_t* restrict node, void* restrict buffer, size_t size, off_t offset)
{
    fat_vnode_context_t* context = node->ctxdata;

    return fcache_read(node, &fat_bmap, buffer, size, offset, context->size_bytes);
}

static ssize_t write_span_locked(vnode_t* restrict node, const void* restrict buffer, size_t size, off_t offset)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    if(fs->readonly) {
        return -EROFS;
    }

    // A FAT file size is a 32-bit field
    if(offset < 0 || (uint64_t)(offset) + size > UINT32_MAX) {
        return -EFBIG;
    }

    ssize_t written = fcache_write(node, &fat_bmap, buffer, size, offset, context->size_bytes);

    if(written <= 0) {
        return written;
    }

    if(offset + written > (off_t)(context->size_bytes)) {
        context->size_bytes = offset + written;
    }

    timespec_t now;
    wallclock_get(&now);
    update_entry_locked(node, nullptr, &now);

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

// Bytes past the end of the file may hold stale data
// in the last cluster, so a gap is zeroed out on disk
static ssize_t write_locked(vnode_t* restrict node, const void* restrict buffer, size_t size, off_t offset)
{
    fat_vnode_context_t* context = node->ctxdata;

    if(offset > context->size_bytes) {
        int retval = zero_fill_locked(node, context->size_bytes, offset);

        if(retval < 0) {
            return retval;
        }
    }

    return write_span_locked(node, buffer, size, offset);
}

static int shrink_locked(vnode_t* restrict node, size_t size)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    // Dirty pages past the new end must not
    // reach clusters that are freed below
    fcache_truncate(node, size);

    size_t keep = (size + fs->cluster_size_b - 1) / fs->cluster_size_b;

    if(keep == 0) {
        free_chain(fs, context->first_cluster);
        context->first_cluster = 0;
    }
    else {
        uintptr_t last = context->first_cluster;

        for(size_t i = 1; i < keep; ++i) {
            last = fat_cluster_next(fs, last);

            if(last == UINTPTR_MAX) {
                return -EIO;
            }
        }

        uintptr_t rest = fat_cluster_next(fs, last);

        if(rest != UINTPTR_MAX) {
            fat_cluster_end(fs, last);
            free_chain(fs, rest);
        }
    }

    context->size_bytes = size;
    context->cached_cluster = context->first_cluster;
    context->cached_cluster_idx = 0;

    if(context->unlinked) {
        return 0;
    }

    fat_dirent_t dirent;
    size_t dir_off = context->dir_offset;
    int retval = fat_dirent_read(fs, context->dir_cluster, &dir_off, &dirent);

    if(retval < 0) {
        return retval;
    }

    dirent.base.file_size = htole32(size);
    dirent.base.first_cluster_hi = htole16(context->first_cluster >> 16);
    dirent.base.first_cluster_lo = htole16(context->first_cluster & 0xFFFF);

    return fat_dirent_update(fs, context->dir_cluster, context->dir_offset, &dirent.base);
}

static int resize_locked(vnode_t* restrict node, off_t size)
{
    fat_vnode_context_t* context = node->ctxdata;

    if(size > UINT32_MAX) {
        return -EFBIG; // A FAT file size is a 32-bit field
    }

    if((size_t)(size) > context->size_bytes) {
        return zero_fill_locked(node, context->size_bytes, size);
    }

    return shrink_locked(node, size);
}

static int set_attr_locked(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    // FAT has nowhere to keep a mode or an owner; with quiet,
    // the change is dropped instead of failing, as with vfat on Linux
    if((attr->mask & (VATTR_MODE | VATTR_UID | VATTR_GID)) && !fs->param_quiet) {
        return -EPERM;
    }

    if(0 == (attr->mask & (VATTR_SIZE | VATTR_ATIME | VATTR_MTIME))) {
        return 0;
    }

    if(fs->readonly) {
        return -EROFS;
    }

    if((attr->mask & VATTR_SIZE) && (size_t)(attr->size) != context->size_bytes) {
        int retval = resize_locked(node, attr->size);

        if(retval < 0) {
            return retval;
        }
    }

    // FAT keeps no change time, so VATTR_CTIME has nowhere to go
    const timespec_t* atime = nullptr;
    const timespec_t* mtime = nullptr;

    if(attr->mask & VATTR_ATIME) {
        atime = &attr->atime;
    }

    if(attr->mask & VATTR_MTIME) {
        mtime = &attr->mtime;
    }

    return update_entry_locked(node, atime, mtime);
}

static int stat_locked(vnode_t* restrict node, stat_t* restrict out)
{
    fat_vnode_context_t* context = node->ctxdata;
    fat_vfs_context_t* fs = context->fs_ctx;

    memset(out, 0, sizeof(stat_t));

    out->st_ino = node->ino;
    out->st_uid = fs->param_uid;
    out->st_gid = fs->param_gid;
    out->st_size = context->size_bytes;
    out->st_blksize = fs->cluster_size_b;

    out->st_blocks = context->size_bytes + fs->sector_size - 1;
    out->st_blocks /= fs->sector_size;

    mode_t rw_bits = S_IRUSR | S_IRGRP | S_IROTH;

    if(!fs->readonly) {
        rw_bits |= S_IWUSR | S_IWGRP;
    }

    mode_t perm_bits = rw_bits | S_IXUSR | S_IXGRP | S_IXOTH;

    if(node->type == VNODE_DIRECTORY) {
        out->st_mode = S_IFDIR | perm_bits;
    }
    else {
        out->st_mode = S_IFREG | perm_bits;
    }

    if(!has_entry(node)) {
        return 0;
    }

    fat_dirent_t dirent;
    size_t dir_off = context->dir_offset;
    int retval = fat_dirent_read(fs, context->dir_cluster, &dir_off, &dirent);

    if(retval < 0) {
        return retval;
    }

    fat_timestamp_t written = {};
    written.date = dirent.base.write_date;
    written.time = dirent.base.write_time;

    fat_timestamp_t accessed = {};
    accessed.date = dirent.base.access_date;

    fat_time_unpack(&written, &out->st_mtim);
    fat_time_unpack(&accessed, &out->st_atim);

    // FAT keeps no change time; vfat on
    // Linux reports the mtime for it too
    out->st_ctim = out->st_mtim;

    return 0;
}

static mutex_t* fs_lock(vnode_t* restrict node)
{
    fat_vnode_context_t* context = node->ctxdata;
    return &context->fs_ctx->lock;
}

static int fat_vnode_lookup(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    mutex_acquire(fs_lock(node));
    int retval = lookup_locked(node, name, out);
    mutex_release(fs_lock(node));
    return retval;
}

static int fat_vnode_iterate(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    mutex_acquire(fs_lock(node));
    int retval = iterate_locked(node, dir_context);
    mutex_release(fs_lock(node));
    return retval;
}

static int fat_vnode_unlink(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    mutex_acquire(fs_lock(node));
    int retval = unlink_locked(node, name, target);
    mutex_release(fs_lock(node));
    return retval;
}

static int fat_vnode_rename(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim)
{
    mutex_acquire(fs_lock(olddir));
    int retval = rename_locked(olddir, oldname, source, newdir, newname, victim);
    mutex_release(fs_lock(olddir));
    return retval;
}

static int fat_vnode_create(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    mutex_acquire(fs_lock(node));
    int retval = create_locked(node, name, type, out);
    mutex_release(fs_lock(node));
    return retval;
}

static ssize_t fat_vnode_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    mutex_acquire(fs_lock(node));
    ssize_t retval = read_locked(node, buffer, size, offset);
    mutex_release(fs_lock(node));
    return retval;
}

static ssize_t fat_vnode_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    mutex_acquire(fs_lock(node));
    ssize_t retval = write_locked(node, buffer, size, offset);
    mutex_release(fs_lock(node));
    return retval;
}

static int fat_vnode_fault(vnode_t* restrict node, off_t offset, pmap_t* restrict pmap, uintptr_t virt, unsigned vprot)
{
    fat_vnode_context_t* context = node->ctxdata;

    mutex_acquire(fs_lock(node));
    int retval = fcache_fault(node, &fat_bmap, offset, context->size_bytes, pmap, virt, vprot);
    mutex_release(fs_lock(node));

    return retval;
}

static int fat_vnode_fsync(vnode_t* restrict node)
{
    fat_vnode_context_t* context = node->ctxdata;

    mutex_acquire(fs_lock(node));
    int retval = fcache_writeback(node, &fat_bmap, context->size_bytes);
    mutex_release(fs_lock(node));
    return retval;
}

static int fat_vnode_stat(vnode_t* restrict node, stat_t* restrict out)
{
    mutex_acquire(fs_lock(node));
    int retval = stat_locked(node, out);
    mutex_release(fs_lock(node));
    return retval;
}

static int fat_vnode_set_attr(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    mutex_acquire(fs_lock(node));
    int retval = set_attr_locked(node, attr);
    mutex_release(fs_lock(node));
    return retval;
}

const vnode_ops_t fat_vnode_ops = {
    .release = &fat_vnode_release,
    .lookup = &fat_vnode_lookup,
    .iterate = &fat_vnode_iterate,
    .create = &fat_vnode_create,
    .unlink = &fat_vnode_unlink,
    .rename = &fat_vnode_rename,
    .read = &fat_vnode_read,
    .write = &fat_vnode_write,
    .stat = &fat_vnode_stat,
    .set_attr = &fat_vnode_set_attr,
    .fsync = &fat_vnode_fsync,
    .fault = &fat_vnode_fault,
};
