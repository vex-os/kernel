#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/fcntl.h>
#include <vex/file_lock.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/mutex.h>
#include <vex/pipe.h>
#include <vex/poll.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/vcache.h>
#include <vex/vnode.h>
#include <vex/wallclock.h>
#include <vex/xattr.h>

#ifdef CONFIG_ATIME_RELATIME
#define RELATIME                  true
#define RELATIME_INTERVAL_SECONDS CONFIG_ATIME_RELATIME_INTERVAL
#else
#define RELATIME                  false
#define RELATIME_INTERVAL_SECONDS 0
#endif

#ifdef CONFIG_ATIME_NONE
#define ATIME_ON_READ false
#else
#define ATIME_ON_READ true
#endif

typedef struct privs_state_s privs_state_t;

struct privs_state_s {
    unsigned seq;
    unsigned caps_state;
    bool clear;
};

static const char* const xattr_prefixes[NUM_XATTR_SPACES] = {
    [XATTR_USER] = "user.",
    [XATTR_TRUSTED] = "trusted.",
    [XATTR_SECURITY] = "security.",
};

static_assert(sizeof(vnode_t) <= SLAB_MAX_SIZE);

static mutex_t rename_mutex = MUTEX_INIT;

static vnode_t* alloc_internal(vfs_t* restrict fs, ino_t ino, const vnode_ops_t* restrict ops)
{
    kassert(fs);
    kassert(ops);

    vnode_t* result = slab_alloc(sizeof(vnode_t));

    if(result == nullptr) {
        return nullptr;
    }

    result->fs = fs;
    result->ops = ops;
    result->ino = ino;

    fcache_init(&result->pages);

    atomic_store(&result->refcount, 1, ATOMIC_RELAXED);

    return result;
}

static int lookup_crossed(vnode_t* restrict node, const char* restrict name, vnode_t** restrict out)
{
    if(node->ops->lookup == nullptr) {
        return -ENOSYS;
    }

    ino_t ino;
    int retval = node->ops->lookup(node, name, &ino);

    if(retval < 0) {
        return retval;
    }

    vnode_t* cached = vcache_get(node->fs, ino);

    if(cached) {
        *out = cached;
        return 0;
    }

    vnode_t* result = alloc_internal(node->fs, ino, node->ops);

    if(result == nullptr) {
        return -ENOMEM;
    }

    if(node->fs->ops->fill_node) {
        retval = node->fs->ops->fill_node(node->fs, result);

        if(retval < 0) {
            slab_free(result);
            return retval;
        }
    }

    if(result->type == VNODE_DIRECTORY) {
        result->mounted_fs = vfs_get_fs(result);
        result->parent = node;
        vnode_incref(node);
    }

    vnode_t* cached_now = vcache_get_or_insert(result);

    if(cached_now != result) {
        vnode_destroy(result);

        if(cached_now == nullptr) {
            return -ENOMEM;
        }
    }

    *out = cached_now;

    return 0;
}

static int cache_created(vnode_t* restrict node, vnode_t* restrict created, vnode_t** restrict out)
{
    if(created->type == VNODE_DIRECTORY && created->parent == nullptr) {
        created->parent = node;
        vnode_incref(node);
    }

    vnode_t* cached_now = vcache_get_or_insert(created);

    if(cached_now != created) {
        vnode_destroy(created);

        if(cached_now == nullptr) {
            return -ENOMEM;
        }
    }

    *out = cached_now;

    return 0;
}

static int check_new_name(vnode_t* restrict node, const char* restrict name)
{
    if(node->fs->readonly) {
        return -EROFS;
    }

    if(0 == strcmp(name, ".") || 0 == strcmp(name, "..")) {
        return -EEXIST;
    }

    return 0;
}

static int link_crossed(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    if(target->fs != node->fs) {
        return -EXDEV;
    }

    if(target->type == VNODE_DIRECTORY || node->ops->link == nullptr) {
        return -EPERM;
    }

    int retval = check_new_name(node, name);

    if(retval < 0) {
        return retval;
    }

    return node->ops->link(node, name, target);
}

static int symlink_crossed(vnode_t* restrict node, const char* restrict name, const char* restrict target,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    if(node->ops->symlink == nullptr) {
        return -EPERM;
    }

    int retval = check_new_name(node, name);

    if(retval < 0) {
        return retval;
    }

    vnode_t* created;
    retval = node->ops->symlink(node, name, target, credentials, &created);

    if(retval < 0) {
        return retval;
    }

    return cache_created(node, created, out);
}

// A FIFO or a device node on a filesystem does its I/O through the code of its kind
static const vnode_ops_t* io_ops(const vnode_t* restrict node)
{
    if(node->special_ops) {
        return node->special_ops;
    }

    return node->ops;
}

// UNDONE: a device node outside devfs finds its interface by rdev, which needs nodev mounts with it.
// Until then such a node only shows its type and rdev, and devfs has the devices
static int device_node_open(vnode_t* restrict node, file_t* restrict file)
{
    return -ENXIO;
}

static const vnode_ops_t device_node_ops = {
    .open = &device_node_open,
};

static int mknod_crossed(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, dev_t rdev,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    if(node->ops->mknod == nullptr) {
        return -EPERM;
    }

    if(node->fs->readonly) {
        return -EROFS;
    }

    vnode_t* created;
    int retval = node->ops->mknod(node, name, type, mode, rdev, credentials, &created);

    if(retval < 0) {
        return retval;
    }

    return cache_created(node, created, out);
}

static int create_crossed(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    if(node->ops->create == nullptr) {
        return -ENOSYS;
    }

    if(node->fs->readonly) {
        return -EROFS;
    }

    vnode_t* created;
    int retval = node->ops->create(node, name, type, mode, credentials, &created);

    if(retval < 0) {
        return retval;
    }

    return cache_created(node, created, out);
}

static int unlink_crossed(vnode_t* restrict node, const char* restrict name, bool directory)
{
    if(node->ops->unlink == nullptr) {
        return -ENOSYS;
    }

    if(node->fs->readonly) {
        return -EROFS;
    }

    if(0 == strcmp(name, ".") || 0 == strcmp(name, "..")) {
        return -EINVAL;
    }

    vnode_t* victim;
    int retval = lookup_crossed(node, name, &victim);

    if(retval < 0) {
        return retval;
    }

    if(directory && victim->type != VNODE_DIRECTORY) {
        vnode_decref(victim);
        return -ENOTDIR;
    }

    if(!directory && victim->type == VNODE_DIRECTORY) {
        vnode_decref(victim);
        return -EISDIR;
    }

    if(victim->mounted_fs) {
        vnode_decref(victim);
        return -EBUSY;
    }

    // Once the victim's last name is gone, the filesystem has to vcache_remove it under its own lock,
    // before the entry can be reused, or a new file there gets this vnode; with names left it stays cached
    retval = node->ops->unlink(node, name, victim);

    // Whichever reference goes last, possibly
    // the one dropped right below, uncounts it
    if(retval == 0 && !vcache_cached(victim)) {
        victim->orphaned = true;
        atomic_fetch_add(&victim->fs->orphans, 1, ATOMIC_RELAXED);
    }

    vnode_decref(victim);

    return retval;
}

static bool is_ancestor(const vnode_t* restrict ancestor, const vnode_t* restrict node)
{
    for(const vnode_t* it = node; it; it = it->parent) {
        if(it == ancestor) {
            return true;
        }
    }

    return false;
}

static void set_parent(vnode_t* restrict node, vnode_t* restrict parent)
{
    vnode_incref(parent);

    spinlock_acquire(&node->lock);
    vnode_t* previous = node->parent;
    node->parent = parent;
    spinlock_release(&node->lock);

    if(previous) {
        vnode_decref(previous);
    }
}

static int check_rename(vnode_t* restrict source, vnode_t* restrict newdir, vnode_t* restrict victim)
{
    if(source->mounted_fs) {
        return -EBUSY;
    }

    if(source->type == VNODE_DIRECTORY && is_ancestor(source, newdir)) {
        return -EINVAL;
    }

    if(victim == nullptr || victim == source) {
        return 0;
    }

    if(victim->mounted_fs) {
        return -EBUSY;
    }

    if(source->type == VNODE_DIRECTORY && victim->type != VNODE_DIRECTORY) {
        return -ENOTDIR;
    }

    if(source->type != VNODE_DIRECTORY && victim->type == VNODE_DIRECTORY) {
        return -EISDIR;
    }

    return 0;
}

static int rename_crossed(vnode_t* olddir, const char* oldname, vnode_t* newdir, const char* newname)
{
    if(olddir->fs != newdir->fs) {
        return -EXDEV;
    }

    if(olddir->ops->rename == nullptr) {
        return -ENOSYS;
    }

    if(olddir->fs->readonly) {
        return -EROFS;
    }

    bool old_dots = 0 == strcmp(oldname, ".") || 0 == strcmp(oldname, "..");
    bool new_dots = 0 == strcmp(newname, ".") || 0 == strcmp(newname, "..");

    if(old_dots || new_dots) {
        return -EINVAL;
    }

    if(oldname[0] == 0 || newname[0] == 0) {
        return -ENOENT;
    }

    mutex_acquire(&rename_mutex);

    vnode_t* source;
    int retval = lookup_crossed(olddir, oldname, &source);

    if(retval < 0) {
        mutex_release(&rename_mutex);
        return retval;
    }

    vnode_t* victim = nullptr;
    retval = lookup_crossed(newdir, newname, &victim);

    if(retval == -ENOENT) {
        victim = nullptr;
        retval = 0;
    }

    if(retval == 0) {
        retval = check_rename(source, newdir, victim);
    }

    bool same_file = victim == source;

    // The filesystem has to vcache_remove a victim left without names and vcache_rekey
    // the source under its own lock, before a lookup can see the new entry
    if(retval == 0 && !same_file) {
        retval = olddir->ops->rename(olddir, oldname, source, newdir, newname, victim);
    }

    if(retval == 0 && !same_file) {
        if(victim && !vcache_cached(victim)) {
            victim->orphaned = true;
            atomic_fetch_add(&victim->fs->orphans, 1, ATOMIC_RELAXED);
        }
    }

    if(retval == 0 && !same_file) {
        if(source->type == VNODE_DIRECTORY && olddir != newdir) {
            set_parent(source, newdir);
        }
    }

    mutex_release(&rename_mutex);

    if(victim) {
        vnode_decref(victim);
    }

    vnode_decref(source);

    return retval;
}

static bool time_not_before(const timespec_t* restrict time, const timespec_t* restrict other)
{
    if(time->tv_sec != other->tv_sec) {
        return time->tv_sec > other->tv_sec;
    }

    return time->tv_nsec >= other->tv_nsec;
}

static bool atime_due(vnode_t* restrict target, const timespec_t* restrict now)
{
    if(!RELATIME) {
        return true;
    }

    stat_t st;

    if(target->ops->stat == nullptr || target->ops->stat(target, &st) < 0) {
        return false;
    }

    bool behind = time_not_before(&st.st_mtim, &st.st_atim) || time_not_before(&st.st_ctim, &st.st_atim);

    return behind || now->tv_sec - st.st_atim.tv_sec >= RELATIME_INTERVAL_SECONDS;
}

static void touch_atime(vnode_t* restrict target)
{
    if(!ATIME_ON_READ || target->fs->readonly || target->ops->set_attr == nullptr) {
        return;
    }

    vnode_attr_t attr = {};
    attr.mask = VATTR_ATIME;
    wallclock_get(&attr.atime);

    if(atime_due(target, &attr.atime)) {
        target->ops->set_attr(target, &attr);
    }
}

static bool time_valid(const timespec_t* restrict time)
{
    if(time->tv_nsec == UTIME_NOW || time->tv_nsec == UTIME_OMIT) {
        return true;
    }

    return time->tv_nsec >= 0 && time->tv_nsec < NSEC_PER_SEC;
}

static bool time_set(const timespec_t* restrict time, const timespec_t* restrict now, timespec_t* restrict out)
{
    if(time->tv_nsec == UTIME_OMIT) {
        return false;
    }

    if(time->tv_nsec == UTIME_NOW) {
        *out = *now;
    }
    else {
        *out = *time;
    }

    return true;
}

static int split_xattr_name(const char* restrict full, unsigned* restrict out_space, const char** restrict out_name)
{
    if(strlen(full) > XATTR_NAME_MAX) {
        return -ERANGE;
    }

    for(unsigned space = 0; space < NUM_XATTR_SPACES; space += 1) {
        size_t length = strlen(xattr_prefixes[space]);

        if(strncmp(full, xattr_prefixes[space], length)) {
            continue;
        }

        if(full[length] == 0) {
            return -EINVAL;
        }

        *out_space = space;
        *out_name = full + length;

        return 0;
    }

    return -EOPNOTSUPP;
}

// Setgid without group execute marks mandatory locking rather than granting anything
static mode_t without_privs(mode_t mode)
{
    mode_t result = mode & ~(S_IFMT | S_ISUID);

    if(mode & S_IXGRP) {
        result &= ~S_ISGID;
    }

    return result;
}

static bool is_caps_name(unsigned space, const char* restrict name)
{
    return space == XATTR_SECURITY && 0 == strcmp(name, XATTR_CAPS_NAME);
}

static void privs_snapshot(vnode_t* restrict node, privs_state_t* restrict out)
{
    spinlock_acquire(&node->lock);
    out->seq = node->privs_seq;
    out->caps_state = node->caps_state;
    out->clear = node->privs_clear;
    spinlock_release(&node->lock);
}

// Comes after the change itself, so that whoever looked
// before it can't record what it saw over the change
static void privs_changed(vnode_t* restrict node)
{
    spinlock_acquire(&node->lock);
    node->privs_seq += 1;
    node->caps_state = VNODE_CAPS_UNKNOWN;
    node->privs_clear = false;
    spinlock_release(&node->lock);
}

static void privs_cleared(vnode_t* restrict node, unsigned seq)
{
    spinlock_acquire(&node->lock);

    if(node->privs_seq == seq) {
        node->caps_state = VNODE_CAPS_NONE;
        node->privs_clear = true;
    }

    spinlock_release(&node->lock);
}

static void caps_learned(vnode_t* restrict node, unsigned seq, unsigned caps_state)
{
    spinlock_acquire(&node->lock);

    if(node->privs_seq == seq) {
        node->caps_state = caps_state;
    }

    spinlock_release(&node->lock);
}

static int require_cap(const credentials_t* restrict credentials, unsigned cap)
{
    if(cred_capable(credentials, cap)) {
        return 0;
    }

    return -EPERM;
}

static int xattr_permission(vnode_t* restrict node, const credentials_t* restrict credentials, unsigned space, const char* restrict name,
    bool write)
{
    if(space == XATTR_TRUSTED) {
        return require_cap(credentials, CAP_SYS_ADMIN);
    }

    if(space == XATTR_SECURITY && !write) {
        return 0;
    }

    if(is_caps_name(space, name)) {
        return require_cap(credentials, CAP_SETFCAP);
    }

    if(space == XATTR_SECURITY) {
        return require_cap(credentials, CAP_SYS_ADMIN);
    }

    // Others could fill up a world-writable device node or symlink
    if(node->type != VNODE_FILE && node->type != VNODE_DIRECTORY) {
        return write ? -EPERM : -ENODATA;
    }

    return vnode_access(node, credentials, write ? W_OK : R_OK);
}

static void strip_file_caps(vnode_t* restrict node)
{
    vnode_t* target;

    if(vfs_cross_mount(node, &target) < 0) {
        return;
    }

    privs_state_t state;
    privs_snapshot(target, &state);

    bool removable = target->type == VNODE_FILE && target->ops->remove_xattr;
    bool writable = !target->fs->readonly && state.caps_state != VNODE_CAPS_NONE;

    if(removable && writable) {
        target->ops->remove_xattr(target, XATTR_SECURITY, XATTR_CAPS_NAME);
        privs_changed(target);
    }

    vnode_decref(target);
}

vnode_t* vnode_alloc(unsigned type, const vnode_ops_t* restrict ops, void* ctxdata)
{
    kassert(ops);

    vnode_t* result = slab_alloc(sizeof(vnode_t));

    if(result == nullptr) {
        return nullptr;
    }

    result->ops = ops;
    result->type = type;
    result->ctxdata = ctxdata;

    fcache_init(&result->pages);

    atomic_store(&result->refcount, 1, ATOMIC_RELAXED);

    return result;
}

void vnode_incref(vnode_t* restrict node)
{
    kassert(node);

    atomic_fetch_add(&node->refcount, 1, ATOMIC_RELAXED);
}

bool vnode_decref(vnode_t* restrict node)
{
    kassert(node);

    unsigned prev = atomic_fetch_sub(&node->refcount, 1, ATOMIC_ACQ_REL);

    if(prev != 1 || vcache_keep(node)) {
        return false;
    }

    vnode_destroy(node);

    return true;
}

void vnode_destroy(vnode_t* restrict node)
{
    kassert(node->cache_entry == nullptr);

    vnode_t* parent = node->parent;

    if(node->orphaned) {
        atomic_fetch_sub(&node->fs->orphans, 1, ATOMIC_RELEASE);
    }

    fcache_deinit(&node->pages);
    file_locks_destroy(node);

    if(node->ops->release)
        node->ops->release(node);
    slab_free(node);

    if(parent) {
        vnode_decref(parent);
    }
}

int vnode_lookup(vnode_t* restrict node, const char* restrict name, vnode_t** restrict out)
{
    kassert(node);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    retval = lookup_crossed(target, name, out);

    vnode_decref(target);

    return retval;
}

bool dir_emit(dir_context_t* restrict dir_context, dirent_t* restrict entry, uint64_t next_position)
{
    entry->d_off = next_position;

    if(!dir_context->emit(dir_context, entry)) {
        return false;
    }

    dir_context->position = next_position;

    return true;
}

uint8_t dirent_type_from_vnode(unsigned vnode_type)
{
    switch(vnode_type) {
        case VNODE_FILE:
            return DT_REG;
        case VNODE_DIRECTORY:
            return DT_DIR;
        case VNODE_BLOCK_DEV:
            return DT_BLK;
        case VNODE_CHAR_DEV:
            return DT_CHR;
        case VNODE_SYM_LINK:
            return DT_LNK;
        case VNODE_FIFO:
            return DT_FIFO;
        default:
            return DT_UNKNOWN;
    }
}

bool dir_emit_dots(dir_context_t* restrict dir_context, ino_t self, ino_t parent)
{
    dirent_t record = {};
    record.d_type = DT_DIR;

    if(dir_context->position == 0) {
        record.d_ino = self;
        record.d_name[0] = '.';

        if(!dir_emit(dir_context, &record, 1)) {
            return false;
        }
    }

    if(dir_context->position == 1) {
        record.d_ino = parent;
        record.d_name[0] = '.';
        record.d_name[1] = '.';

        if(!dir_emit(dir_context, &record, DIR_DOTS_END)) {
            return false;
        }
    }

    return true;
}

int vnode_iterate(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    kassert(node);

    if(node->type != VNODE_DIRECTORY) {
        return -ENOTDIR;
    }

    bool from_start = dir_context->position == 0;

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    if(target->ops->iterate == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = target->ops->iterate(target, dir_context);
    }

    if(retval == 0 && from_start) {
        touch_atime(target);
    }

    vnode_decref(target);

    return retval;
}

void vnode_init_special(vnode_t* restrict node, dev_t rdev)
{
    node->rdev = rdev;

    if(node->type == VNODE_FIFO) {
        node->special_ops = &pipe_fifo_ops;
    }
    else {
        node->special_ops = &device_node_ops;
    }
}

int vnode_mknod(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, dev_t rdev,
    const credentials_t* restrict credentials, vnode_t** restrict out)
{
    kassert(node);
    kassert(credentials);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    retval = mknod_crossed(target, name, type, mode & ~S_IFMT, rdev, credentials, out);

    vnode_decref(target);

    return retval;
}

int vnode_create(vnode_t* restrict node, const char* restrict name, unsigned type, mode_t mode, const credentials_t* restrict credentials,
    vnode_t** restrict out)
{
    kassert(node);
    kassert(credentials);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    retval = create_crossed(target, name, type, mode & ~S_IFMT, credentials, out);

    vnode_decref(target);

    return retval;
}

int vnode_unlink(vnode_t* restrict node, const char* restrict name, bool directory)
{
    kassert(node);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    retval = unlink_crossed(target, name, directory);

    vnode_decref(target);

    return retval;
}

int vnode_rename(vnode_t* olddir, const char* oldname, vnode_t* newdir, const char* newname)
{
    kassert(olddir);
    kassert(newdir);

    vnode_t* source_dir;
    int retval = vfs_cross_mount(olddir, &source_dir);

    if(retval < 0) {
        return retval;
    }

    vnode_t* target_dir;
    retval = vfs_cross_mount(newdir, &target_dir);

    if(retval < 0) {
        vnode_decref(source_dir);
        return retval;
    }

    retval = rename_crossed(source_dir, oldname, target_dir, newname);

    vnode_decref(target_dir);
    vnode_decref(source_dir);

    return retval;
}

int vnode_link(vnode_t* restrict node, const char* restrict name, vnode_t* restrict target)
{
    kassert(node);
    kassert(target);

    vnode_t* crossed;
    int retval = vfs_cross_mount(node, &crossed);

    if(retval < 0) {
        return retval;
    }

    retval = link_crossed(crossed, name, target);

    vnode_decref(crossed);

    return retval;
}

int vnode_symlink(vnode_t* restrict node, const char* restrict name, const char* restrict target, const credentials_t* restrict credentials,
    vnode_t** restrict out)
{
    kassert(node);
    kassert(credentials);

    if(target[0] == 0) {
        return -ENOENT;
    }

    vnode_t* crossed;
    int retval = vfs_cross_mount(node, &crossed);

    if(retval < 0) {
        return retval;
    }

    retval = symlink_crossed(crossed, name, target, credentials, out);

    vnode_decref(crossed);

    return retval;
}

int vnode_open(vnode_t* restrict node, file_t* restrict file)
{
    kassert(node);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    const vnode_ops_t* ops = io_ops(target);

    if(ops->open == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = ops->open(target, file);
    }

    vnode_decref(target);

    return retval;
}

int vnode_close(vnode_t* restrict node, file_t* restrict file)
{
    kassert(node);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    const vnode_ops_t* ops = io_ops(target);

    if(ops->close == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = ops->close(target, file);
    }

    vnode_decref(target);

    return retval;
}

ssize_t vnode_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    kassert(node);

    vnode_t* target;
    ssize_t retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    const vnode_ops_t* ops = io_ops(target);

    if(ops->read == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = ops->read(target, file, buffer, size, offset);
    }

    if(retval >= 0) {
        touch_atime(target);
    }

    vnode_decref(target);

    return retval;
}

ssize_t vnode_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    kassert(node);

    vnode_t* target;
    ssize_t retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    const vnode_ops_t* ops = io_ops(target);

    if(ops->write == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = ops->write(target, file, buffer, size, offset);
    }

    vnode_decref(target);

    return retval;
}

off_t vnode_seek(vnode_t* restrict node, file_t* restrict file, off_t offset, int origin)
{
    kassert(node);

    vnode_t* target;
    off_t retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    const vnode_ops_t* ops = io_ops(target);

    if(ops->seek == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = ops->seek(target, file, offset, origin);
    }

    vnode_decref(target);

    return retval;
}

int vnode_stat(vnode_t* restrict node, stat_t* restrict out)
{
    kassert(node);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    if(target->ops->stat == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = target->ops->stat(target, out);
    }

    if(retval == 0) {
        out->st_dev = target->fs->dev;
    }

    vnode_decref(target);

    return retval;
}

int vnode_set_attr(vnode_t* restrict node, const vnode_attr_t* restrict request)
{
    kassert(node);
    kassert(request);

    if((request->mask & VATTR_SIZE) && (node->type != VNODE_FILE || request->size < 0)) {
        return -EINVAL;
    }

    vnode_attr_t attr = *request;

    if(attr.mask && 0 == (attr.mask & VATTR_CTIME)) {
        attr.mask |= VATTR_CTIME;
        wallclock_get(&attr.ctime);
    }

    if((attr.mask & VATTR_SIZE) && 0 == (attr.mask & VATTR_MTIME)) {
        attr.mask |= VATTR_MTIME;
        attr.mtime = attr.ctime;
    }

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    if(target->fs->readonly) {
        retval = -EROFS;
    }
    else if(target->ops->set_attr == nullptr) {
        retval = -EPERM;
    }
    else {
        retval = target->ops->set_attr(target, &attr);
    }

    if(retval == 0 && (attr.mask & (VATTR_MODE | VATTR_UID | VATTR_GID))) {
        privs_changed(target);
    }

    vnode_decref(target);

    return retval;
}

int vnode_chmod(vnode_t* restrict node, const credentials_t* restrict credentials, mode_t mode)
{
    kassert(node);
    kassert(credentials);

    stat_t st;
    int retval = vnode_stat(node, &st);

    if(retval < 0) {
        return retval;
    }

    if(credentials->euid != st.st_uid && !cred_capable(credentials, CAP_FOWNER)) {
        return -EPERM;
    }

    vnode_attr_t attr = {};
    attr.mask = VATTR_MODE;
    attr.mode = mode & (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO);

    // Otherwise anyone could hand out a group they are not in
    if(!cred_in_group(credentials, st.st_gid) && !cred_capable(credentials, CAP_FSETID)) {
        attr.mode &= ~S_ISGID;
    }

    return vnode_set_attr(node, &attr);
}

int vnode_chown(vnode_t* restrict node, const credentials_t* restrict credentials, uid_t uid, gid_t gid)
{
    kassert(node);
    kassert(credentials);

    if(uid < -1 || gid < -1) {
        return -EINVAL;
    }

    stat_t st;
    int retval = vnode_stat(node, &st);

    if(retval < 0) {
        return retval;
    }

    vnode_attr_t attr = {};

    if(uid != -1 && uid != st.st_uid) {
        attr.mask |= VATTR_UID;
        attr.uid = uid;
    }

    if(gid != -1 && gid != st.st_gid) {
        attr.mask |= VATTR_GID;
        attr.gid = gid;
    }

    if(!cred_capable(credentials, CAP_CHOWN)) {
        // The owner may only move the file
        // into another group of its own
        if(attr.mask & VATTR_UID) {
            return -EPERM;
        }

        if((attr.mask & VATTR_GID) && (credentials->euid != st.st_uid || !cred_in_group(credentials, gid))) {
            return -EPERM;
        }
    }

    if(attr.mask == 0) {
        return 0;
    }

    // A program must not keep running as an owner it no longer has
    if((st.st_mode & S_IFMT) != S_IFDIR && without_privs(st.st_mode) != (st.st_mode & ~S_IFMT)) {
        attr.mask |= VATTR_MODE;
        attr.mode = without_privs(st.st_mode);
    }

    retval = vnode_set_attr(node, &attr);

    if(retval == 0) {
        strip_file_caps(node);
    }

    return retval;
}

int vnode_access(vnode_t* restrict node, const credentials_t* restrict credentials, unsigned mask)
{
    kassert(node);
    kassert(credentials);

    stat_t st;
    int retval = vnode_stat(node, &st);

    if(retval < 0) {
        return retval;
    }

    if(cred_capable(credentials, CAP_DAC_OVERRIDE)) {
        if((mask & X_OK) && (st.st_mode & S_IFMT) != S_IFDIR && 0 == (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            return -EACCES;
        }

        return 0;
    }

    mode_t granted;

    if(credentials->euid == st.st_uid) {
        granted = (st.st_mode >> 6) & 7;
    }
    else if(cred_in_group(credentials, st.st_gid)) {
        granted = (st.st_mode >> 3) & 7;
    }
    else {
        granted = st.st_mode & 7;
    }

    if((granted & mask) != mask) {
        return -EACCES;
    }

    return 0;
}

int vnode_remove_privs(vnode_t* restrict node, const credentials_t* restrict credentials)
{
    kassert(node);
    kassert(credentials);

    if(node->type != VNODE_FILE) {
        return 0;
    }

    privs_state_t state;
    privs_snapshot(node, &state);

    if(state.clear) {
        return 0;
    }

    stat_t st;
    int retval = vnode_stat(node, &st);

    if(retval < 0) {
        return retval;
    }

    mode_t stripped = without_privs(st.st_mode);
    bool mode_clear = stripped == (st.st_mode & ~S_IFMT);

    if(!mode_clear && !cred_capable(credentials, CAP_FSETID)) {
        vnode_attr_t attr = {};
        attr.mask = VATTR_MODE;
        attr.mode = stripped;
        retval = vnode_set_attr(node, &attr);
    }

    bool caps_clear = state.caps_state == VNODE_CAPS_NONE || node->ops->remove_xattr == nullptr;

    if(retval == 0 && !caps_clear) {
        retval = node->ops->remove_xattr(node, XATTR_SECURITY, XATTR_CAPS_NAME);

        if(retval == 0) {
            privs_changed(node);
        }

        caps_clear = retval == -ENODATA;
    }

    if(retval == -ENODATA) {
        retval = 0;
    }

    // Whatever was taken away above moved the sequence on,
    // so only the next write marks the file as clean
    if(retval == 0 && mode_clear && caps_clear) {
        privs_cleared(node, state.seq);
    }

    return retval;
}

int vnode_utimes(vnode_t* restrict node, const credentials_t* restrict credentials, const timespec_t* restrict times)
{
    kassert(node);
    kassert(credentials);

    static const timespec_t both_now[2] = {
        { .tv_nsec = UTIME_NOW },
        { .tv_nsec = UTIME_NOW },
    };

    if(times == nullptr) {
        times = both_now;
    }

    if(!time_valid(&times[0]) || !time_valid(&times[1])) {
        return -EINVAL;
    }

    vnode_attr_t attr = {};
    timespec_t now;
    wallclock_get(&now);

    if(time_set(&times[0], &now, &attr.atime)) {
        attr.mask |= VATTR_ATIME;
    }

    if(time_set(&times[1], &now, &attr.mtime)) {
        attr.mask |= VATTR_MTIME;
    }

    if(attr.mask == 0) {
        return 0;
    }

    attr.mask |= VATTR_CTIME;
    attr.ctime = now;

    stat_t st;
    int retval = vnode_stat(node, &st);

    if(retval < 0) {
        return retval;
    }

    // Anyone who may write the file may touch it, but
    // only its owner may set the times to something else
    if(credentials->euid != st.st_uid && !cred_capable(credentials, CAP_FOWNER)) {
        bool now_only = times[0].tv_nsec != UTIME_OMIT || times[1].tv_nsec != UTIME_OMIT;
        now_only = now_only && (times[0].tv_nsec == UTIME_NOW || times[0].tv_nsec == UTIME_OMIT);
        now_only = now_only && (times[1].tv_nsec == UTIME_NOW || times[1].tv_nsec == UTIME_OMIT);

        if(!now_only) {
            return -EPERM;
        }

        retval = vnode_access(node, credentials, W_OK);

        if(retval < 0) {
            return retval;
        }
    }

    return vnode_set_attr(node, &attr);
}

int vnode_may_delete(vnode_t* restrict dir, const char* restrict name, const credentials_t* restrict credentials)
{
    kassert(dir);
    kassert(name);
    kassert(credentials);

    int retval = vnode_access(dir, credentials, W_OK | X_OK);

    if(retval < 0) {
        return retval;
    }

    stat_t dir_st;
    retval = vnode_stat(dir, &dir_st);

    if(retval < 0) {
        return retval;
    }

    // In a sticky directory such as /tmp, write access
    // alone lets nobody remove what others put there
    if(0 == (dir_st.st_mode & S_ISVTX) || credentials->euid == dir_st.st_uid || cred_capable(credentials, CAP_FOWNER)) {
        return 0;
    }

    vnode_t* victim;

    // Nothing there to protect; the operation itself reports a missing name
    if(vnode_lookup(dir, name, &victim) < 0) {
        return 0;
    }

    stat_t victim_st;
    retval = vnode_stat(victim, &victim_st);

    vnode_decref(victim);

    if(retval < 0) {
        return retval;
    }

    if(credentials->euid != victim_st.st_uid) {
        return -EPERM;
    }

    return 0;
}

ssize_t vnode_readlink(vnode_t* restrict node, char* restrict buf, size_t size)
{
    kassert(node);

    if(node->ops->readlink == nullptr)
        return -ENOSYS;
    return node->ops->readlink(node, buf, size);
}

int vnode_ioctl(vnode_t* restrict node, file_t* restrict file, uint32_t request, uintptr_t argument)
{
    kassert(node);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    const vnode_ops_t* ops = io_ops(target);

    if(ops->ioctl == nullptr) {
        retval = -ENOSYS;
    }
    else {
        retval = ops->ioctl(target, file, request, argument);
    }

    vnode_decref(target);

    return retval;
}

unsigned vnode_poll(vnode_t* restrict node, file_t* restrict file, poll_table_t* restrict table)
{
    kassert(node);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return POLLERR;
    }

    unsigned revents = POLL_READABLE | POLL_WRITABLE;

    const vnode_ops_t* ops = io_ops(target);

    if(ops->poll) {
        revents = ops->poll(target, file, table);
    }

    vnode_decref(target);

    return revents;
}

int vnode_fsync(vnode_t* restrict node)
{
    kassert(node);

    if(node->ops->fsync == nullptr) {
        return 0;
    }

    return node->ops->fsync(node);
}

int vnode_fallocate(vnode_t* restrict node, unsigned mode, off_t offset, off_t length)
{
    kassert(node);

    if(node->ops->fallocate == nullptr) {
        return -EOPNOTSUPP;
    }

    return node->ops->fallocate(node, mode, offset, length);
}

int vnode_fallocate_grow(vnode_t* restrict node, unsigned mode, off_t offset, off_t length)
{
    if(mode) {
        return -EOPNOTSUPP;
    }

    stat_t stat;
    int retval = vnode_stat(node, &stat);

    if(retval < 0) {
        return retval;
    }

    off_t end = offset + length;

    if(end <= stat.st_size) {
        return 0;
    }

    vnode_attr_t attr = {};
    attr.mask = VATTR_SIZE;
    attr.size = end;

    return vnode_set_attr(node, &attr);
}

int vnode_fault(vnode_t* restrict node, off_t offset, pmap_t* restrict pmap, virt_addr_t virt, unsigned vprot)
{
    kassert(node);

    if(node->ops->fault == nullptr) {
        return -ENODEV;
    }

    return node->ops->fault(node, offset, pmap, virt, vprot);
}

int vnode_mmap(vnode_t* restrict node, off_t offset, size_t length)
{
    kassert(node);

    if(node->ops->mmap == nullptr) {
        return 0;
    }

    return node->ops->mmap(node, offset, length);
}

int vnode_resolve_page(vnode_t* restrict node, off_t offset, phys_addr_t* restrict phys, unsigned* restrict cache)
{
    kassert(node);

    if(node->ops->resolve_page == nullptr) {
        return -ENODEV;
    }

    return node->ops->resolve_page(node, offset, phys, cache);
}

ssize_t vnode_get_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, const char* restrict name, void* restrict value,
    size_t size)
{
    kassert(node);
    kassert(credentials);

    unsigned space;
    const char* suffix;
    ssize_t retval = split_xattr_name(name, &space, &suffix);

    if(retval < 0) {
        return retval;
    }

    vnode_t* target;
    retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    retval = xattr_permission(target, credentials, space, suffix, false);

    if(retval == 0 && target->ops->get_xattr == nullptr) {
        retval = -EOPNOTSUPP;
    }

    if(retval == 0) {
        retval = target->ops->get_xattr(target, space, suffix, value, size);
    }

    vnode_decref(target);

    return retval;
}

int vnode_set_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, const char* restrict name,
    const void* restrict value, size_t size, unsigned flags)
{
    kassert(node);
    kassert(credentials);

    if((flags & ~(XATTR_CREATE | XATTR_REPLACE)) || flags == (XATTR_CREATE | XATTR_REPLACE)) {
        return -EINVAL;
    }

    if(size > XATTR_SIZE_MAX) {
        return -E2BIG;
    }

    unsigned space;
    const char* suffix;
    int retval = split_xattr_name(name, &space, &suffix);

    if(retval < 0) {
        return retval;
    }

    vnode_t* target;
    retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    if(target->fs->readonly) {
        retval = -EROFS;
    }

    if(retval == 0) {
        retval = xattr_permission(target, credentials, space, suffix, true);
    }

    if(retval == 0 && target->ops->set_xattr == nullptr) {
        retval = -EOPNOTSUPP;
    }

    if(retval == 0) {
        retval = target->ops->set_xattr(target, space, suffix, value, size, flags);

        if(is_caps_name(space, suffix)) {
            privs_changed(target);
        }
    }

    vnode_decref(target);

    return retval;
}

ssize_t vnode_list_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, char* restrict list, size_t size)
{
    kassert(node);
    kassert(credentials);

    vnode_t* target;
    int retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    bool trusted = cred_capable(credentials, CAP_SYS_ADMIN);
    size_t length = 0;

    for(size_t index = 0; target->ops->iterate_xattr; index += 1) {
        xattr_entry_t entry;
        retval = target->ops->iterate_xattr(target, &entry, index);

        if(retval < 0) {
            break;
        }

        kassert(entry.space < NUM_XATTR_SPACES);

        if(entry.space == XATTR_TRUSTED && !trusted) {
            continue;
        }

        const char* prefix = xattr_prefixes[entry.space];
        size_t prefix_len = strlen(prefix);
        size_t name_len = strlen(entry.name) + 1;

        if(size && length + prefix_len + name_len > size) {
            retval = -ERANGE;
            break;
        }

        if(size) {
            memcpy(list + length, prefix, prefix_len);
            memcpy(list + length + prefix_len, entry.name, name_len);
        }

        length += prefix_len + name_len;
    }

    vnode_decref(target);

    if(retval < 0 && retval != -ENOENT) {
        return retval;
    }

    return length;
}

int vnode_remove_xattr(vnode_t* restrict node, const credentials_t* restrict credentials, const char* restrict name)
{
    kassert(node);
    kassert(credentials);

    unsigned space;
    const char* suffix;
    int retval = split_xattr_name(name, &space, &suffix);

    if(retval < 0) {
        return retval;
    }

    vnode_t* target;
    retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    if(target->fs->readonly) {
        retval = -EROFS;
    }

    if(retval == 0) {
        retval = xattr_permission(target, credentials, space, suffix, true);
    }

    if(retval == 0 && target->ops->remove_xattr == nullptr) {
        retval = -EOPNOTSUPP;
    }

    if(retval == 0) {
        retval = target->ops->remove_xattr(target, space, suffix);

        if(is_caps_name(space, suffix)) {
            privs_changed(target);
        }
    }

    vnode_decref(target);

    return retval;
}

ssize_t vnode_get_caps(vnode_t* restrict node, void* restrict value, size_t size)
{
    kassert(node);

    vnode_t* target;
    ssize_t retval = vfs_cross_mount(node, &target);

    if(retval < 0) {
        return retval;
    }

    privs_state_t state;
    privs_snapshot(target, &state);

    if(state.caps_state == VNODE_CAPS_NONE || target->ops->get_xattr == nullptr) {
        retval = -ENODATA;
    }
    else {
        retval = target->ops->get_xattr(target, XATTR_SECURITY, XATTR_CAPS_NAME, value, size);
    }

    if(retval == -ENODATA) {
        caps_learned(target, state.seq, VNODE_CAPS_NONE);
    }
    else if(retval >= 0 || retval == -ERANGE) {
        caps_learned(target, state.seq, VNODE_CAPS_PRESENT);
    }

    vnode_decref(target);

    return retval;
}
