#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/page.h>
#include <vex/shmem.h>
#include <vex/sprintf.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/vnode.h>

// The filesystem has to count the pages of a
// file against the commit limit when its size grows

#define SHMEM_FS_TYPE    "tmpfs"
#define SHMEM_FILE_MODE  (S_IRUSR | S_IWUSR)
#define NUMBER_NAME_SIZE 24 // Fits the digits of a 64-bit number

static vfs_t* shmem_fs = nullptr;
static uint64_t next_name = 0;

// The file is made with a name and loses it at once. Nothing
// else can reach the filesystem, so the name is only the way in
static int make_unnamed(vnode_t** restrict out)
{
    uint64_t number = atomic_fetch_add(&next_name, 1, ATOMIC_RELAXED);
    char name[NUMBER_NAME_SIZE];

    snprintf(name, sizeof(name), "%llu", (unsigned long long)(number));

    vnode_t* root = shmem_fs->root;
    vnode_t* node;
    int retval = vnode_create(root, name, VNODE_FILE, SHMEM_FILE_MODE, &kernel_credentials, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_unlink(root, name, false);

    if(retval < 0) {
        vnode_decref(node);
        return retval;
    }

    *out = node;

    return 0;
}

int shmem_file_create(size_t size, file_t** restrict out)
{
    kassert(size && page_offset(size) == 0);

    if(shmem_fs == nullptr) {
        return -ENOSYS;
    }

    if(size > INT64_MAX) {
        return -ENOMEM;
    }

    vnode_t* node;
    int retval = make_unnamed(&node);

    if(retval < 0) {
        return retval;
    }

    vnode_attr_t attr = {};
    attr.mask = VATTR_SIZE;
    attr.size = size;
    retval = vnode_set_attr(node, &attr);

    if(retval < 0) {
        vnode_decref(node);

        if(retval == -ENOSPC) {
            return -ENOMEM;
        }

        return retval;
    }

    // Takes over the reference to the node
    return file_open_node(node, O_RDWR, &kernel_credentials, out);
}

static void init_shmem(void)
{
    int retval = vfs_create(SHMEM_FS_TYPE, nullptr, &shmem_fs);

    if(retval < 0) {
        pr_warning("shmem: no %s: %s", SHMEM_FS_TYPE, strerror(retval));
    }
}

initcall_bsp_fs(init_shmem, INIT_EARLY);
