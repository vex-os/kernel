#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/block_interface.h>
#include <vex/cmdline.h>
#include <vex/dev_name.h>
#include <vex/fcache.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/mount.h>
#include <vex/mutex.h>
#include <vex/path.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/vcache.h>
#include <vex/vnode.h>

typedef struct mount_entry_s mount_entry_t;

struct mount_entry_s {
    mount_entry_t* next;
    vfs_t* fs;
    char* path;
};

static_assert(sizeof(vfs_t) <= SLAB_MAX_SIZE);
static_assert(sizeof(vfs_driver_t) <= SLAB_MAX_SIZE);

static spinlock_t drivers_lock = SPINLOCK_INIT;
static vfs_driver_t* drivers_head = nullptr;

// The list only changes with both held, so either one is enough to walk it
static mutex_t mounts_mutex = MUTEX_INIT;
static spinlock_t mounts_lock = SPINLOCK_INIT;
static mount_entry_t* mounts_head = nullptr;
static vnode_t* vfs_root = nullptr;

static vfs_driver_t* find_driver(const char* restrict name)
{
    vfs_driver_t* it;

    for(it = drivers_head; it; it = it->priv.next) {
        if(0 == strcmp(it->name, name)) {
            return it;
        }
    }

    return nullptr;
}

// A probe reads the device and may sleep, so the lock is not held for it.
// A driver that is removed meanwhile ends the walk, as removal clears its link
static vfs_driver_t* deduce_driver(const block_interface_t* restrict volume)
{
    spinlock_acquire(&drivers_lock);

    vfs_driver_t* it = drivers_head;

    while(it) {
        const vfs_ops_t* ops = it->ops;

        spinlock_release(&drivers_lock);

        if(ops->probe && ops->probe(nullptr, volume) == 0) {
            return it;
        }

        spinlock_acquire(&drivers_lock);
        it = it->priv.next;
    }

    spinlock_release(&drivers_lock);

    return nullptr;
}

static int set_dev(vfs_t* restrict fs, const block_interface_t* restrict volume)
{
    fs->unit = nullptr;

    if(volume) {
        fs->dev = volume->priv.dev;
        return 0;
    }

    int retval = dev_unit_alloc(&dev_prefix_anon, nullptr, &fs->unit);

    if(retval < 0) {
        return retval;
    }

    dev_name_t name = {};
    name.prefix = fs->unit->prefix;
    name.unit = fs->unit->number;
    name.index = DEV_NAME_NONE;
    name.partition = DEV_NAME_NONE;

    fs->dev = dev_name_to_dev(&name);

    return 0;
}

static void free_fs(vfs_t* restrict fs)
{
    if(fs->unit) {
        dev_unit_free(fs->unit);
    }

    slab_free(fs);
}

static void params_parse(cmdline_t* restrict cmdl, const char* restrict params)
{
    cmdl->buffer = nullptr;
    cmdl->length = 0;

    if(params) {
        cmdl->buffer = kstrdup(params);

        if(cmdl->buffer) {
            parse_cmdline(cmdl, cmdl->buffer, 1 + strlen(params));
        }
    }
}

static void params_free(cmdline_t* restrict cmdl)
{
    if(cmdl->buffer) {
        kfree(cmdl->buffer);
    }
}

// Makes the filesystem and lets the driver mount it. The label only names it in the log
static int build_fs(const vfs_driver_t* restrict driver, const block_interface_t* restrict volume, unsigned flags,
    const cmdline_t* restrict cmdl, const char* restrict label, vfs_t** restrict out)
{
    vfs_t* fs = slab_alloc(sizeof(vfs_t));

    if(fs == nullptr) {
        return -ENOMEM;
    }

    fs->ops = driver->ops;
    fs->readonly = (flags & MNT_RDONLY) || cmdline_contains(cmdl, "ro") || cmdline_contains(cmdl, "readonly");

    int retval = set_dev(fs, volume);

    if(retval < 0) {
        slab_free(fs);
        return retval;
    }

    // The driver reads its metadata
    // through the file cache while mounting
    fs->volume = volume;

    retval = driver->ops->mount(fs, volume, cmdl);

    const char* device_name;

    if(volume == nullptr) {
        device_name = "null";
    }
    else {
        device_name = volume->name;
    }

    if(retval == 0) {
        pr_verbose("vfs: mount %s: mountpoint=%s device=%s", driver->name, label, device_name);
    }
    else {
        const char* reason = strerror(retval);

        pr_verbose("vfs: %s: failed to mount: %s; mountpoint=%s device=%s", driver->name, reason, label, device_name);
    }

    if(retval < 0) {
        free_fs(fs);
        return retval;
    }

    *out = fs;

    return 0;
}

static int mount_locked(const char* restrict type, const char* restrict path, const block_interface_t* restrict volume, unsigned flags,
    const char* restrict params)
{
    // Without a type the driver is found by probing the device
    if(path == nullptr || (type == nullptr && volume == nullptr)) {
        return -EINVAL;
    }

    vfs_driver_t* driver;

    if(type == nullptr) {
        driver = deduce_driver(volume);
    }
    else {
        spinlock_acquire(&drivers_lock);
        driver = find_driver(type);
        spinlock_release(&drivers_lock);
    }

    if(driver == nullptr) {
        return -ENOENT;
    }

    if(driver->internal) {
        return -ENODEV;
    }

    cmdline_t cmdl;
    params_parse(&cmdl, params);

    vfs_t* fs;
    int retval = build_fs(driver, volume, flags, &cmdl, path, &fs);

    params_free(&cmdl);

    if(retval < 0) {
        return retval;
    }

    vnode_t* mountpoint = nullptr;

    if(path[0] != PATH_SEPARATOR || path[1]) {
        retval = path_resolve(path, &kernel_credentials, &mountpoint);

        if(retval < 0) {
            vcache_evict(fs);

            driver->ops->unmount(fs);

            free_fs(fs);

            return retval;
        }
    }

    spinlock_acquire(&mounts_lock);

    if(mountpoint == nullptr) {
        vfs_root = fs->root;
        vnode_incref(vfs_root);
    }
    else {
        mountpoint->mounted_fs = fs;
        fs->mountpoint = mountpoint;
    }

    mount_entry_t* entry = slab_alloc(sizeof(mount_entry_t));

    if(entry == nullptr) {
        spinlock_release(&mounts_lock);

        vcache_evict(fs);

        driver->ops->unmount(fs);

        free_fs(fs);

        return -ENOMEM;
    }

    entry->path = kstrdup(path);

    if(entry->path == nullptr) {
        spinlock_release(&mounts_lock);

        vcache_evict(fs);

        driver->ops->unmount(fs);

        slab_free(entry);
        free_fs(fs);

        return -ENOMEM;
    }

    entry->fs = fs;
    entry->next = mounts_head;
    mounts_head = entry;

    spinlock_release(&mounts_lock);

    if(volume) {
        block_interface_incref(volume);
    }

    return 0;
}

int vfs_driver_add(vfs_driver_t* restrict driver)
{
    if(driver == nullptr || driver->ops == nullptr) {
        return -EINVAL;
    }

    if(driver->ops->mount == nullptr || driver->ops->unmount == nullptr) {
        return -EINVAL;
    }

    if(driver->name == nullptr || driver->name[0] == 0) {
        return -EINVAL;
    }

    spinlock_acquire(&drivers_lock);

    if(find_driver(driver->name)) {
        spinlock_release(&drivers_lock);
        return -EEXIST;
    }

    driver->priv.next = drivers_head;
    drivers_head = driver;

    spinlock_release(&drivers_lock);

    return 0;
}

int vfs_driver_remove(vfs_driver_t* restrict driver)
{
    vfs_driver_t** prev;

    if(driver == nullptr) {
        return -EINVAL;
    }

    spinlock_acquire(&drivers_lock);

    for(prev = &drivers_head; *prev; prev = &((**prev).priv.next)) {
        if(*prev == driver) {
            *prev = driver->priv.next;

            driver->priv.next = nullptr;

            spinlock_release(&drivers_lock);

            return 0;
        }
    }

    spinlock_release(&drivers_lock);

    return -ENOENT;
}

int vfs_create(const char* restrict type, const char* restrict params, vfs_t** restrict out)
{
    kassert(type);
    kassert(out);

    spinlock_acquire(&drivers_lock);
    vfs_driver_t* driver = find_driver(type);
    spinlock_release(&drivers_lock);

    if(driver == nullptr) {
        return -ENOENT;
    }

    cmdline_t cmdl;
    params_parse(&cmdl, params);

    int retval = build_fs(driver, nullptr, 0, &cmdl, "(internal)", out);

    params_free(&cmdl);

    return retval;
}

int vfs_mount(const char* restrict type, const char* restrict path, const block_interface_t* restrict volume, unsigned flags,
    const char* restrict params)
{
    mutex_acquire(&mounts_mutex);
    int retval = mount_locked(type, path, volume, flags, params);
    mutex_release(&mounts_mutex);
    return retval;
}

int vfs_unmount(const char* restrict path)
{
    kassert(path);

    mutex_acquire(&mounts_mutex);

    mount_entry_t** prev = &mounts_head;

    while(*prev && (*prev)->path && strcmp((*prev)->path, path)) {
        prev = &(*prev)->next;
    }

    mount_entry_t* entry = *prev;

    if(entry == nullptr) {
        mutex_release(&mounts_mutex);
        return -ENOENT;
    }

    vfs_t* fs = entry->fs;
    bool is_root = vfs_root == fs->root;

    // Operations that crossed in before the flag was set
    // hold a reference for their whole duration, so the check sees them
    spinlock_acquire(&mounts_lock);
    fs->unmounting = true;
    spinlock_release(&mounts_lock);

    // Vnodes queued for writeback are referenced by the queue
    int retval = fcache_sync(fs);

    if(retval == 0 && (atomic_load(&fs->orphans, ATOMIC_ACQUIRE) || vcache_in_use(fs, is_root ? 2 : 1))) {
        retval = -EBUSY;
    }

    if(retval == 0) {
        retval = vfs_sync_fs(fs);

        if(retval == -ENOSYS) {
            retval = 0;
        }
    }

    if(retval < 0) {
        spinlock_acquire(&mounts_lock);
        fs->unmounting = false;
        spinlock_release(&mounts_lock);

        mutex_release(&mounts_mutex);

        return retval;
    }

    spinlock_acquire(&mounts_lock);

    *prev = entry->next;

    vnode_t* mountpoint = fs->mountpoint;

    if(mountpoint) {
        mountpoint->mounted_fs = nullptr;
        fs->mountpoint = nullptr;
    }

    if(is_root) {
        vfs_root = nullptr;
    }

    spinlock_release(&mounts_lock);

    pr_verbose("vfs: unmounting %s", path);

    if(mountpoint) {
        vnode_decref(mountpoint);
    }

    if(is_root) {
        vnode_decref(fs->root);
    }

    vcache_evict(fs);
    fs->ops->unmount(fs);

    if(fs->volume) {
        block_interface_decref(fs->volume);
    }

    free_fs(fs);
    kfree(entry->path);
    slab_free(entry);

    mutex_release(&mounts_mutex);

    return 0;
}

int vfs_sync_fs(vfs_t* restrict fs)
{
    int retval = fcache_sync(fs);

    if(fs->ops->sync == nullptr) {
        return retval < 0 ? retval : -ENOSYS;
    }

    int fs_retval = fs->ops->sync(fs);

    return retval < 0 ? retval : fs_retval;
}

int vfs_sync_all(void)
{
    int retval = 0;

    mutex_acquire(&mounts_mutex);

    for(mount_entry_t* entry = mounts_head; entry; entry = entry->next) {
        int fs_retval = vfs_sync_fs(entry->fs);

        if(fs_retval < 0 && fs_retval != -ENOSYS && retval == 0) {
            retval = fs_retval;
        }
    }

    mutex_release(&mounts_mutex);

    return retval;
}

int vfs_statvfs(vfs_t* restrict fs, statvfs_t* restrict out)
{
    kassert(fs);
    kassert(out);

    if(fs->ops->statvfs == nullptr) {
        return -ENOSYS;
    }

    int retval = fs->ops->statvfs(fs, out);

    if(retval == 0) {
        out->f_fsid = (uint32_t)(fs->dev ^ (fs->dev >> 32)); // f_fsid is only 32 bits wide
    }

    return retval;
}

vnode_t* vfs_get_root(void)
{
    spinlock_acquire(&mounts_lock);

    if(vfs_root == nullptr || vfs_root->fs->unmounting) {
        spinlock_release(&mounts_lock);
        return nullptr;
    }

    vnode_incref(vfs_root);

    spinlock_release(&mounts_lock);

    return vfs_root;
}

vnode_t* vfs_get_mountpoint(vfs_t* restrict fs)
{
    kassert(fs);

    spinlock_acquire(&mounts_lock);

    vnode_t* mountpoint = fs->mountpoint;

    if(mountpoint) {
        vnode_incref(mountpoint);
    }

    spinlock_release(&mounts_lock);

    return mountpoint;
}

vfs_t* vfs_get_fs(vnode_t* restrict node)
{
    kassert(node);

    spinlock_acquire(&mounts_lock);

    for(mount_entry_t* it = mounts_head; it; it = it->next) {
        vnode_t* mountpoint = it->fs->mountpoint;

        if(mountpoint == nullptr) {
            continue;
        }

        if(mountpoint == node) {
            vfs_t* result = it->fs;
            spinlock_release(&mounts_lock);
            return result;
        }
    }

    spinlock_release(&mounts_lock);

    return nullptr;
}

int vfs_cross_mount(vnode_t* restrict node, vnode_t** restrict out)
{
    kassert(node);
    kassert(out);

    if(node->type != VNODE_DIRECTORY) {
        vnode_incref(node);
        *out = node;
        return 0;
    }

    spinlock_acquire(&mounts_lock);

    vfs_t* fs = node->mounted_fs;

    if(fs && fs->unmounting) {
        spinlock_release(&mounts_lock);
        return -EBUSY;
    }

    if(fs) {
        node = fs->root;
    }

    vnode_incref(node);

    spinlock_release(&mounts_lock);

    *out = node;

    return 0;
}
