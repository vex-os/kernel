#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/page.h>
#include <vex/pslab.h>
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

// Only a target in a page counts, as a slab object is as small as the node that holds it
static size_t target_pages(size_t length)
{
    if(length > SLAB_MAX_SIZE) {
        return 1;
    }

    return 0;
}

static mode_t type_bits(unsigned type)
{
    switch(type) {
        case VNODE_DIRECTORY:
            return S_IFDIR;
        case VNODE_SYM_LINK:
            return S_IFLNK;
        case VNODE_FIFO:
            return S_IFIFO;
        case VNODE_CHAR_DEV:
            return S_IFCHR;
        case VNODE_BLOCK_DEV:
            return S_IFBLK;
        default:
            return S_IFREG;
    }
}

static bool is_special(unsigned type)
{
    return type == VNODE_FIFO || type == VNODE_CHAR_DEV || type == VNODE_BLOCK_DEV;
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

static char* target_alloc(const char* restrict target, size_t length)
{
    char* copy;

    if(length > SLAB_MAX_SIZE) {
        copy = pslab_alloc_virt();
    }
    else {
        copy = slab_alloc(length);
    }

    if(copy) {
        memcpy(copy, target, length);
    }

    return copy;
}

static void target_free(char* restrict target, size_t length)
{
    if(length > SLAB_MAX_SIZE) {
        pslab_free_virt(target);
    }
    else {
        slab_free(target);
    }
}

// A symlink takes the target, a copy of length bytes, and owns it only if this succeeds
static int create_locked(vnode_t* restrict dir, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, char* restrict target, size_t length, vnode_t** restrict out)
{
    tmpfs_inode_t* parent = dir->ctxdata;
    bool directory = type == VNODE_DIRECTORY;
    bool symlink = type == VNODE_SYM_LINK;
    bool known = type == VNODE_FILE || directory || symlink;

    if(!known && !is_special(type)) {
        return -EINVAL;
    }

    if(symlink != (bool)(target)) {
        return -EINVAL;
    }

    if(strlen(name) > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    // A directory removed while open has no name to put entries under
    if(parent->nlink == 0) {
        return -ENOENT;
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

    if(symlink) {
        tmpfs_inode_t* inode = node->ctxdata;
        inode->target = target;
        inode->size = length;
    }

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

    if(node->type == VNODE_SYM_LINK) {
        tmpfs_uncharge(tmpfs, target_pages(inode->size));
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

    if(inode->target) {
        target_free(inode->target, inode->size);
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
    int retval = create_locked(node, name, type, mode, credentials, nullptr, 0, &created);
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

static int tmpfs_vnode_mknod(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, dev_t rdev,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    if(!is_special(type)) {
        return -EINVAL;
    }

    tmpfs_t* tmpfs = tmpfs_of(node);
    vnode_t* created = nullptr;

    mutex_acquire(&tmpfs->lock);
    int retval = create_locked(node, name, type, mode, credentials, nullptr, 0, &created);

    if(retval == 0) {
        tmpfs_inode_t* inode = created->ctxdata;
        inode->rdev = rdev;
        vnode_init_special(created, rdev);
    }

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

static int tmpfs_vnode_link(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* dir = node->ctxdata;
    tmpfs_inode_t* inode = target->ctxdata;

    if(strlen(name) > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    tmpfs_entry_t* entry = entry_alloc(name);

    if(entry == nullptr) {
        return -ENOMEM;
    }

    mutex_acquire(&tmpfs->lock);

    int retval = 0;

    // A file unlinked while open has no name to link to
    if(inode->nlink == 0 || dir->nlink == 0) {
        retval = -ENOENT;
    }
    else if(find_entry(dir, name)) {
        retval = -EEXIST;
    }

    if(retval == 0) {
        entry->node = target;
        entry_link(dir, entry);
        inode->nlink += 1;

        touch(inode, false);
        touch(dir, true);
    }

    mutex_release(&tmpfs->lock);

    if(retval < 0) {
        slab_free(entry);
    }

    return retval;
}

static int tmpfs_vnode_symlink(vnode_t* restrict node, const char* restrict name, const char* restrict target,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    tmpfs_t* tmpfs = tmpfs_of(node);
    size_t length = strlen(target);

    // The target and its path must fit the path buffer of a system call
    if(length >= PATH_MAX) {
        return -ENAMETOOLONG;
    }

    char* copy = target_alloc(target, length);

    if(copy == nullptr) {
        return -ENOMEM;
    }

    vnode_t* created = nullptr;

    size_t pages = target_pages(length);

    mutex_acquire(&tmpfs->lock);

    int retval = tmpfs_charge(tmpfs, pages);

    if(retval == 0) {
        retval = create_locked(node, name, VNODE_SYM_LINK, S_IRWXU | S_IRWXG | S_IRWXO, credentials, copy, length, &created);

        if(retval < 0) {
            tmpfs_uncharge(tmpfs, pages);
        }
    }

    mutex_release(&tmpfs->lock);

    if(retval < 0) {
        target_free(copy, length);

        if(created) {
            vnode_decref(created);
        }

        return retval;
    }

    *out = created;

    return 0;
}

// Moves the name of source. Returns in nameless whether victim lost its last name
static int rename_locked(vnode_t* olddir, const char* restrict oldname, vnode_t* restrict source, vnode_t* newdir,
    const char* restrict newname, vnode_t* restrict victim, tmpfs_entry_t* restrict entry, bool* restrict nameless)
{
    tmpfs_inode_t* old_parent = olddir->ctxdata;
    tmpfs_inode_t* new_parent = newdir->ctxdata;
    tmpfs_inode_t* moved = source->ctxdata;
    tmpfs_entry_t* old_entry = find_entry(old_parent, oldname);
    tmpfs_entry_t* new_entry = find_entry(new_parent, newname);

    if(old_entry == nullptr || old_entry->node != source || new_parent->nlink == 0) {
        return -ENOENT;
    }

    if(victim == nullptr && new_entry) {
        return -EEXIST;
    }

    if(victim && (new_entry == nullptr || new_entry->node != victim)) {
        return -ENOENT;
    }

    tmpfs_inode_t* lost = nullptr;

    if(victim) {
        lost = victim->ctxdata;

        if(victim->type == VNODE_DIRECTORY && lost->first_entry) {
            return -ENOTEMPTY;
        }
    }

    // Nothing fails from here on
    if(victim) {
        entry_unlink(new_parent, new_entry);
        slab_free(new_entry);
    }

    entry_unlink(old_parent, old_entry);
    slab_free(old_entry);

    entry->node = source;
    entry_link(new_parent, entry);

    // The links of a directory come from the ".." of its subdirectories
    if(source->type == VNODE_DIRECTORY && old_parent != new_parent) {
        old_parent->nlink -= 1;
        new_parent->nlink += 1;
    }

    if(victim) {
        if(victim->type == VNODE_DIRECTORY) {
            lost->nlink = 0;
            new_parent->nlink -= 1;
        }
        else {
            lost->nlink -= 1;
        }

        *nameless = lost->nlink == 0;

        if(*nameless) {
            vcache_remove(victim);
        }

        touch(lost, false);
    }

    touch(old_parent, true);
    touch(new_parent, true);
    touch(moved, false);

    return 0;
}

static int tmpfs_vnode_rename(vnode_t* olddir, const char* oldname, vnode_t* source, vnode_t* newdir, const char* newname, vnode_t* victim)
{
    tmpfs_t* tmpfs = tmpfs_of(olddir);

    if(strlen(newname) > NAME_MAX) {
        return -ENAMETOOLONG;
    }

    tmpfs_entry_t* entry = entry_alloc(newname);

    if(entry == nullptr) {
        return -ENOMEM;
    }

    bool nameless = false;

    mutex_acquire(&tmpfs->lock);
    int retval = rename_locked(olddir, oldname, source, newdir, newname, victim, entry, &nameless);
    mutex_release(&tmpfs->lock);

    if(retval < 0) {
        slab_free(entry);
        return retval;
    }

    // The caller holds a reference, so the node stays until it lets go
    if(nameless) {
        vnode_decref(victim);
    }

    return 0;
}

static ssize_t tmpfs_vnode_readlink(vnode_t* restrict node, char* restrict buf, size_t size)
{
    const tmpfs_inode_t* inode = node->ctxdata;

    if(node->type != VNODE_SYM_LINK) {
        return -EINVAL;
    }

    // The target never changes, so it needs no lock
    if(size > (size_t)(inode->size)) {
        size = inode->size;
    }

    memcpy(buf, inode->target, size);

    return size;
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
    out->st_rdev = inode->rdev;
    out->st_blksize = PAGE_SIZE;

    if(node->type == VNODE_FILE) {
        out->st_blocks = pages_of(inode->size) * (PAGE_SIZE / S_BLKSIZE);
    }

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

// The commit limit counts the whole size of a file, holes included, so a file that is long enough has its pages paid for
// already. There is nothing to reserve past the end, and so no KEEP_SIZE
static int tmpfs_vnode_fallocate(vnode_t* restrict node, unsigned mode, off_t offset, off_t length)
{
    if(mode) {
        return -EOPNOTSUPP;
    }

    tmpfs_t* tmpfs = tmpfs_of(node);
    tmpfs_inode_t* inode = node->ctxdata;
    off_t end = offset + length;
    int retval = 0;

    mutex_acquire(&tmpfs->lock);

    bool grows = end > inode->size;

    if(grows) {
        retval = resize_locked(node, inode, end);
    }

    if(retval == 0 && grows) {
        touch(inode, true);
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
    .mknod = &tmpfs_vnode_mknod,
    .unlink = &tmpfs_vnode_unlink,
    .rename = &tmpfs_vnode_rename,
    .link = &tmpfs_vnode_link,
    .symlink = &tmpfs_vnode_symlink,
    .readlink = &tmpfs_vnode_readlink,
    .read = &tmpfs_vnode_read,
    .write = &tmpfs_vnode_write,
    .stat = &tmpfs_vnode_stat,
    .set_attr = &tmpfs_vnode_set_attr,
    .fallocate = &tmpfs_vnode_fallocate,
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
