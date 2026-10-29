#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/page.h>
#include <vex/slab.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/vcache.h>
#include <vex/vnode.h>
#include <vex/wallclock.h>

#include "tmpfs_internal.h"

#define MODE_MASK (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO)

static_assert(sizeof(tmpfs_inode_t) <= SLAB_MAX_SIZE);
static_assert(sizeof(tmpfs_entry_t) + NAME_SIZE <= SLAB_MAX_SIZE);

static tmpfs_t* tmpfs_of(const vnode_t* restrict node)
{
    const tmpfs_inode_t* inode = node->ctxdata;
    return inode->tmpfs;
}

static size_t pages_of(off_t size)
{
    return ((size_t)(size) + PAGE_SIZE - 1) / PAGE_SIZE;
}

static mode_t type_bits(unsigned type)
{
    if(type == VNODE_DIRECTORY) {
        return S_IFDIR;
    }

    if(type == VNODE_SYM_LINK) {
        return S_IFLNK;
    }

    return S_IFREG;
}

static void touch(tmpfs_inode_t* restrict inode, bool modified)
{
    timespec_t now;
    wallclock_get(&now);

    inode->ctime = now;

    if(modified) {
        inode->mtime = now;
    }
}

static tmpfs_entry_t* find_entry(const tmpfs_inode_t* restrict dir, const char* restrict name)
{
    for(tmpfs_entry_t* entry = dir->first_entry; entry; entry = entry->next) {
        if(0 == strcmp(entry->name, name)) {
            return entry;
        }
    }

    return nullptr;
}

static tmpfs_entry_t* entry_alloc(const char* restrict name)
{
    size_t length = strlen(name);
    tmpfs_entry_t* entry = slab_alloc(sizeof(tmpfs_entry_t) + length + 1);

    if(entry == nullptr) {
        return nullptr;
    }

    memset(entry, 0, sizeof(tmpfs_entry_t));
    memcpy(entry->name, name, length + 1);

    return entry;
}

static void entry_link(tmpfs_inode_t* restrict dir, tmpfs_entry_t* restrict entry)
{
    entry->seq = dir->next_seq;
    dir->next_seq += 1;

    entry->prev = dir->last_entry;
    entry->next = nullptr;

    if(dir->last_entry) {
        dir->last_entry->next = entry;
    }
    else {
        dir->first_entry = entry;
    }

    dir->last_entry = entry;
}

static void entry_unlink(tmpfs_inode_t* restrict dir, tmpfs_entry_t* restrict entry)
{
    if(entry->prev) {
        entry->prev->next = entry->next;
    }
    else {
        dir->first_entry = entry->next;
    }

    if(entry->next) {
        entry->next->prev = entry->prev;
    }
    else {
        dir->last_entry = entry->prev;
    }
}

// Pages past the end of the file hold zeroes, so a longer file shows no old data
static int resize_locked(vnode_t* restrict node, tmpfs_inode_t* restrict inode, off_t new_size)
{
    size_t old_pages = pages_of(inode->size);
    size_t new_pages = pages_of(new_size);

    if(new_size > inode->size) {
        if(new_pages > old_pages) {
            int retval = tmpfs_charge(inode->tmpfs, new_pages - old_pages);

            if(retval < 0) {
                return retval;
            }
        }

        fcache_truncate(node, inode->size);
    }
    else if(new_size < inode->size) {
        fcache_truncate(node, new_size);
        tmpfs_uncharge(inode->tmpfs, old_pages - new_pages);
    }

    inode->size = new_size;

    return 0;
}

static int create_locked(vnode_t* restrict dir, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    tmpfs_inode_t* parent = dir->ctxdata;
    bool directory = type == VNODE_DIRECTORY;

    if(type != VNODE_FILE && !directory) {
        return -EINVAL;
    }

    if(strlen(name) > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    if(find_entry(parent, name)) {
        return -EEXIST;
    }

    // A set-group-ID directory hands down its
    // group, and the bit itself to subdirectories
    gid_t gid = credentials->egid;
    mode &= MODE_MASK;

    if(parent->mode & S_ISGID) {
        gid = parent->gid;

        if(directory) {
            mode |= S_ISGID;
        }
    }

    tmpfs_entry_t* entry = entry_alloc(name);

    if(entry == nullptr) {
        return -ENOMEM;
    }

    vnode_t* node;
    int retval = tmpfs_node_alloc(dir->fs, type, mode, credentials->euid, gid, &node);

    if(retval < 0) {
        slab_free(entry);
        return retval;
    }

    // The caller drops the node on failure
    *out = node;

    retval = vcache_insert_pinned(node);

    if(retval < 0) {
        slab_free(entry);
        return retval;
    }

    entry->node = node;
    entry_link(parent, entry);

    if(directory) {
        node->parent = dir;
        vnode_incref(dir);
        parent->nlink += 1;
    }

    touch(parent, true);

    return 0;
}

static void tmpfs_vnode_release(vnode_t* restrict node)
{
    tmpfs_inode_t* inode = node->ctxdata;
    tmpfs_t* tmpfs = inode->tmpfs;

    mutex_acquire(&tmpfs->lock);

    if(inode->all_prev) {
        inode->all_prev->all_next = inode->all_next;
    }
    else {
        tmpfs->inodes = inode->all_next;
    }

    if(inode->all_next) {
        inode->all_next->all_prev = inode->all_prev;
    }

    if(node->type == VNODE_FILE) {
        tmpfs_uncharge(tmpfs, pages_of(inode->size));
    }

    tmpfs->used_nodes -= 1;

    mutex_release(&tmpfs->lock);

    // Only an unmount leaves names behind, and they hold no references
    tmpfs_entry_t* entry = inode->first_entry;

    while(entry) {
        tmpfs_entry_t* next = entry->next;
        slab_free(entry);
        entry = next;
    }

    slab_free(inode);
}

static int tmpfs_vnode_lookup(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    tmpfs_t* tmpfs = tmpfs_of(node);

    if(node->type != VNODE_DIRECTORY) {
        return -ENOTDIR;
    }

    mutex_acquire(&tmpfs->lock);

    tmpfs_entry_t* entry = find_entry(node->ctxdata, name);
    int retval = -ENOENT;

    if(entry) {
        vnode_t* found = entry->node;
        *out = found->ino;
        retval = 0;
    }

    mutex_release(&tmpfs->lock);

    return retval;
}

static int tmpfs_vnode_iterate(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* inode = node->ctxdata;
    vnode_t* parent = node->parent;
    dirent_t record;

    if(parent == nullptr) {
        parent = node;
    }

    if(!dir_emit_dots(dir_context, node->ino, parent->ino)) {
        return 0;
    }

    mutex_acquire(&tmpfs->lock);

    for(tmpfs_entry_t* entry = inode->first_entry; entry; entry = entry->next) {
        if(entry->seq < dir_context->position) {
            continue;
        }

        vnode_t* found = entry->node;

        memset(record.d_name, 0, sizeof(record.d_name));
        kstrncpy(record.d_name, entry->name, sizeof(record.d_name));
        record.d_ino = found->ino;
        record.d_type = dirent_type_from_vnode(found->type);

        if(!dir_emit(dir_context, &record, entry->seq + 1)) {
            break;
        }
    }

    mutex_release(&tmpfs->lock);

    return 0;
}

static int tmpfs_vnode_create(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    vnode_t* created = nullptr;

    mutex_acquire(&tmpfs->lock);
    int retval = create_locked(node, name, type, mode, credentials, &created);
    mutex_release(&tmpfs->lock);

    if(retval < 0) {
        if(created) {
            vnode_decref(created);
        }

        return retval;
    }

    *out = created;

    return 0;
}

static int tmpfs_vnode_unlink(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* dir = node->ctxdata;
    tmpfs_inode_t* inode = target->ctxdata;
    bool directory = target->type == VNODE_DIRECTORY;

    mutex_acquire(&tmpfs->lock);

    tmpfs_entry_t* entry = find_entry(dir, name);

    if(entry == nullptr || entry->node != target) {
        mutex_release(&tmpfs->lock);
        return -ENOENT;
    }

    if(directory && inode->first_entry) {
        mutex_release(&tmpfs->lock);
        return -ENOTEMPTY;
    }

    entry_unlink(dir, entry);
    slab_free(entry);

    if(directory) {
        inode->nlink = 0;
        dir->nlink -= 1; // The ".." of the removed directory
    }
    else {
        inode->nlink -= 1;
    }

    touch(dir, true);
    touch(inode, false);

    bool nameless = inode->nlink == 0;

    if(nameless) {
        vcache_remove(target);
    }

    mutex_release(&tmpfs->lock);

    // The caller holds a reference, so the node stays until it lets go
    if(nameless) {
        vnode_decref(target);
    }

    return 0;
}

static ssize_t tmpfs_vnode_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* inode = node->ctxdata;

    if(node->type == VNODE_DIRECTORY) {
        return -EISDIR;
    }

    if(node->type != VNODE_FILE) {
        return -EINVAL;
    }

    mutex_acquire(&tmpfs->lock);
    ssize_t retval = fcache_read(node, nullptr, buffer, size, offset, inode->size);
    mutex_release(&tmpfs->lock);

    return retval;
}

static ssize_t tmpfs_vnode_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* inode = node->ctxdata;

    if(node->type == VNODE_DIRECTORY) {
        return -EISDIR;
    }

    if(node->type != VNODE_FILE || offset < 0) {
        return -EINVAL;
    }

    if(size == 0) {
        return 0;
    }

    if(size > (size_t)(INT64_MAX - offset)) {
        return -EFBIG;
    }

    off_t end = offset + size;
    ssize_t written;

    mutex_acquire(&tmpfs->lock);

    off_t old_size = inode->size;
    int retval = 0;

    // The file pays for its new size first, so a full filesystem fails the write whole
    if(end > old_size) {
        retval = resize_locked(node, inode, end);
    }

    if(retval < 0) {
        written = retval;
    }
    else {
        written = fcache_write(node, nullptr, buffer, size, offset, inode->size);

        // A short write keeps only the pages it filled
        if(end > old_size) {
            off_t kept = old_size;

            if(written > 0 && (off_t)(offset + written) > kept) {
                kept = offset + written;
            }

            if(kept < end) {
                resize_locked(node, inode, kept);
            }
        }

        if(written > 0) {
            touch(inode, true);
        }
    }

    mutex_release(&tmpfs->lock);

    return written;
}

static int tmpfs_vnode_stat(vnode_t* restrict node, stat_t* restrict out)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    const tmpfs_inode_t* inode = node->ctxdata;

    memset(out, 0, sizeof(stat_t));

    mutex_acquire(&tmpfs->lock);

    out->st_ino = node->ino;
    out->st_mode = type_bits(node->type) | inode->mode;
    out->st_nlink = inode->nlink;
    out->st_uid = inode->uid;
    out->st_gid = inode->gid;
    out->st_size = inode->size;
    out->st_atim = inode->atime;
    out->st_mtim = inode->mtime;
    out->st_ctim = inode->ctime;
    out->st_blksize = PAGE_SIZE;
    out->st_blocks = pages_of(inode->size) * (PAGE_SIZE / 512);

    mutex_release(&tmpfs->lock);

    return 0;
}

static int tmpfs_vnode_set_attr(vnode_t* restrict node, const vnode_attr_t* restrict attr)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* inode = node->ctxdata;
    int retval = 0;

    mutex_acquire(&tmpfs->lock);

    // The size comes first, as it is the only change that can fail
    if(attr->mask & VATTR_SIZE) {
        retval = resize_locked(node, inode, attr->size);
    }

    if(retval == 0) {
        if(attr->mask & VATTR_MODE) {
            inode->mode = attr->mode & MODE_MASK;
        }

        if(attr->mask & VATTR_UID) {
            inode->uid = attr->uid;
        }

        if(attr->mask & VATTR_GID) {
            inode->gid = attr->gid;
        }

        if(attr->mask & VATTR_ATIME) {
            inode->atime = attr->atime;
        }

        if(attr->mask & VATTR_MTIME) {
            inode->mtime = attr->mtime;
        }

        if(attr->mask & VATTR_CTIME) {
            inode->ctime = attr->ctime;
        }
    }

    mutex_release(&tmpfs->lock);

    return retval;
}

static int tmpfs_vnode_fault(vnode_t* restrict node, off_t offset, pmap_t* restrict pmap, virt_addr_t virt, unsigned vprot)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* inode = node->ctxdata;

    if(node->type != VNODE_FILE) {
        return -ENODEV;
    }

    mutex_acquire(&tmpfs->lock);
    int retval = fcache_fault(node, nullptr, offset, inode->size, pmap, virt, vprot);
    mutex_release(&tmpfs->lock);

    return retval;
}

const vnode_ops_t tmpfs_vnode_ops = {
    .release = &tmpfs_vnode_release,
    .lookup = &tmpfs_vnode_lookup,
    .iterate = &tmpfs_vnode_iterate,
    .create = &tmpfs_vnode_create,
    .unlink = &tmpfs_vnode_unlink,
    .read = &tmpfs_vnode_read,
    .write = &tmpfs_vnode_write,
    .stat = &tmpfs_vnode_stat,
    .set_attr = &tmpfs_vnode_set_attr,
    .fault = &tmpfs_vnode_fault,
};

int tmpfs_node_alloc(vfs_t* restrict fs, unsigned type, mode_t mode, uid_t uid, gid_t gid, vnode_t** restrict out)
{
    tmpfs_t* tmpfs = fs->ctxdata;

    if(tmpfs->used_nodes >= tmpfs->max_nodes) {
        return -ENOSPC;
    }

    tmpfs_inode_t* inode = slab_alloc(sizeof(tmpfs_inode_t));

    if(inode == nullptr) {
        return -ENOMEM;
    }

    memset(inode, 0, sizeof(tmpfs_inode_t));

    vnode_t* node = vnode_alloc(type, &tmpfs_vnode_ops, inode);

    if(node == nullptr) {
        slab_free(inode);
        return -ENOMEM;
    }

    node->fs = fs;
    node->ino = tmpfs->next_ino;
    tmpfs->next_ino += 1;
    fcache_set_memory(&node->pages);

    inode->tmpfs = tmpfs;
    inode->node = node;
    inode->mode = mode;
    inode->uid = uid;
    inode->gid = gid;
    inode->nlink = 1;

    if(type == VNODE_DIRECTORY) {
        inode->nlink = 2;
        inode->next_seq = DIR_DOTS_END;
    }

    wallclock_get(&inode->atime);
    inode->mtime = inode->atime;
    inode->ctime = inode->atime;

    inode->all_next = tmpfs->inodes;

    if(tmpfs->inodes) {
        tmpfs->inodes->all_prev = inode;
    }

    tmpfs->inodes = inode;
    tmpfs->used_nodes += 1;

    *out = node;

    return 0;
}
