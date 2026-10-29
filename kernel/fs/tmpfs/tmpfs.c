#include <vex/cmdline.h>
#include <vex/commit.h>
#include <vex/errno.h>
#include <vex/hrsize.h>
#include <vex/initcall.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/page.h>
#include <vex/slab.h>
#include <vex/stat.h>
#include <vex/stdlib.h>
#include <vex/string.h>
#include <vex/vnode.h>

#include "tmpfs_internal.h"

static_assert(sizeof(tmpfs_t) <= SLAB_MAX_SIZE);

#define ROOT_MODE (S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO)

static bool parse_number(const char* restrict string, int base, unsigned long long* restrict out)
{
    const char* end;
    unsigned long long value = strtoull(string, &end, base);

    if(end == string || *end) {
        return false;
    }

    *out = value;

    return true;
}

static int parse_options(tmpfs_t* restrict tmpfs, const cmdline_t* restrict params, mode_t* restrict mode, uid_t* restrict uid,
    gid_t* restrict gid)
{
    unsigned long long value;
    const char* size_string = cmdline_argument(params, "size");
    const char* nodes_string = cmdline_argument(params, "nodes");
    const char* mode_string = cmdline_argument(params, "mode");
    const char* uid_string = cmdline_argument(params, "uid");
    const char* gid_string = cmdline_argument(params, "gid");

    if(size_string) {
        size_t bytes;

        if(parse_size_iec(size_string, &bytes) < 0) {
            return -EINVAL;
        }

        tmpfs->max_pages = bytes / PAGE_SIZE;
    }

    if(nodes_string) {
        if(!parse_number(nodes_string, 10, &value)) {
            return -EINVAL;
        }

        tmpfs->max_nodes = value;
    }

    if(mode_string) {
        if(!parse_number(mode_string, 8, &value) || value > 07777) {
            return -EINVAL;
        }

        *mode = value;
    }

    if(uid_string) {
        if(!parse_number(uid_string, 10, &value)) {
            return -EINVAL;
        }

        *uid = value;
    }

    if(gid_string) {
        if(!parse_number(gid_string, 10, &value)) {
            return -EINVAL;
        }

        *gid = value;
    }

    return 0;
}

static int tmpfs_mount(vfs_t* restrict fs, const block_interface_t* restrict volume, const cmdline_t* restrict params)
{
    if(volume) {
        return -EINVAL;
    }

    tmpfs_t* tmpfs = slab_alloc(sizeof(tmpfs_t));

    if(tmpfs == nullptr) {
        return -ENOMEM;
    }

    memset(tmpfs, 0, sizeof(tmpfs_t));
    mutex_init(&tmpfs->lock);

    tmpfs->next_ino = TMPFS_ROOT_INO;
    tmpfs->max_pages = SIZE_MAX;
    tmpfs->max_nodes = SIZE_MAX;

    mode_t mode = ROOT_MODE;
    uid_t uid = 0;
    gid_t gid = 0;

    int retval = parse_options(tmpfs, params, &mode, &uid, &gid);

    if(retval == 0) {
        fs->ctxdata = tmpfs;
        retval = tmpfs_node_alloc(fs, VNODE_DIRECTORY, mode, uid, gid, &fs->root);
    }

    if(retval < 0) {
        mutex_deinit(&tmpfs->lock);
        slab_free(tmpfs);
        return retval;
    }

    return 0;
}

static int tmpfs_unmount(vfs_t* restrict fs)
{
    tmpfs_t* tmpfs = fs->ctxdata;

    // Nothing else touches the filesystem now. A node that is still listed
    // holds the reference of its name, and the node after it holds one as well
    tmpfs_inode_t* inode = tmpfs->inodes;

    while(inode) {
        tmpfs_inode_t* next = inode->all_next;

        if(inode->node != fs->root) {
            vnode_decref(inode->node);
        }

        inode = next;
    }

    vnode_decref(fs->root);

    mutex_deinit(&tmpfs->lock);
    slab_free(tmpfs);

    return 0;
}

static int tmpfs_statvfs(vfs_t* restrict fs, statvfs_t* restrict out)
{
    tmpfs_t* tmpfs = fs->ctxdata;

    memset(out, 0, sizeof(statvfs_t));

    size_t charged = atomic_load(&commit_charged_pages, ATOMIC_RELAXED);
    size_t system_free = 0;

    if(commit_limit_pages > charged) {
        system_free = commit_limit_pages - charged;
    }

    mutex_acquire(&tmpfs->lock);

    size_t total = tmpfs->used_pages + system_free;
    size_t free_pages = system_free;

    if(tmpfs->max_pages != SIZE_MAX) {
        total = tmpfs->max_pages;
        free_pages = tmpfs->max_pages - tmpfs->used_pages;

        if(free_pages > system_free) {
            free_pages = system_free;
        }
    }

    size_t total_nodes = tmpfs->used_nodes + system_free;

    if(tmpfs->max_nodes != SIZE_MAX) {
        total_nodes = tmpfs->max_nodes;
    }

    out->f_bsize = PAGE_SIZE;
    out->f_frsize = PAGE_SIZE;
    out->f_blocks = total;
    out->f_bfree = free_pages;
    out->f_bavail = free_pages;
    out->f_files = total_nodes;
    out->f_ffree = total_nodes - tmpfs->used_nodes;
    out->f_favail = out->f_ffree;
    out->f_namemax = NAME_MAX;

    mutex_release(&tmpfs->lock);

    if(fs->readonly) {
        out->f_flag |= ST_RDONLY;
    }

    return 0;
}

static const vfs_ops_t tmpfs_ops = {
    .mount = &tmpfs_mount,
    .statvfs = &tmpfs_statvfs,
    .unmount = &tmpfs_unmount,
};

static vfs_driver_t tmpfs_driver = {
    .name = "tmpfs",
    .ops = &tmpfs_ops,
};

int tmpfs_charge(tmpfs_t* restrict tmpfs, size_t pages)
{
    if(pages > tmpfs->max_pages - tmpfs->used_pages) {
        return -ENOSPC;
    }

    if(commit_charge(pages) < 0) {
        return -ENOSPC;
    }

    tmpfs->used_pages += pages;

    return 0;
}

void tmpfs_uncharge(tmpfs_t* restrict tmpfs, size_t pages)
{
    commit_uncharge(pages);
    tmpfs->used_pages -= pages;
}

static void init_tmpfs(void)
{
    vfs_driver_add(&tmpfs_driver);
}

initcall_bsp_vfs(init_tmpfs, INIT_EARLY);
