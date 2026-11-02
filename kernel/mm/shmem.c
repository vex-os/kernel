#include <vex/assert.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/mount.h>
#include <vex/page.h>
#include <vex/shmem.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/vnode.h>

// The filesystem has to count the pages of a
// file against the commit limit when its size grows

#define SHMEM_FS_TYPE   "tmpfs"
#define SHMEM_FILE_MODE (S_IRUSR | S_IWUSR)

static vfs_t* shmem_fs = nullptr;

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
    int retval = vnode_tmpfile(shmem_fs->root, SHMEM_FILE_MODE, &kernel_credentials, &node);

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
