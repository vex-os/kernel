#include <vex/atomic.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/mount.h>
#include <vex/path.h>
#include <vex/slab.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/vnode.h>

// O_RDWR is both bits together, so each one
// stands for read and write access on its own
static unsigned access_for(unsigned flags)
{
    unsigned mask = 0;

    if(flags & O_RDONLY) {
        mask |= R_OK;
    }

    if(flags & (O_WRONLY | O_TRUNC)) {
        mask |= W_OK;
    }

    return mask;
}

int file_open_node(vnode_t* restrict node, unsigned flags, const credentials_t* restrict credentials, file_t** restrict out)
{
    if((flags & O_DIRECTORY) && node->type != VNODE_DIRECTORY) {
        vnode_decref(node);
        return -ENOTDIR;
    }

    // A symlink only gets here when the path stopped at it without following
    if((flags & O_NOFOLLOW) && node->type == VNODE_SYM_LINK) {
        vnode_decref(node);
        return -ELOOP;
    }

    if(node->type == VNODE_DIRECTORY && (flags & (O_WRONLY | O_TRUNC))) {
        vnode_decref(node);
        return -EISDIR;
    }

    // Device nodes stay writable, the device isn't the filesystem they live on
    if(node->type == VNODE_FILE && (flags & (O_WRONLY | O_TRUNC)) && node->fs->readonly) {
        vnode_decref(node);
        return -EROFS;
    }

    file_t* file = slab_alloc(sizeof(file_t));

    if(file == nullptr) {
        vnode_decref(node);
        return -ENOMEM;
    }

    file->node = node;
    file->flags = flags;
    file->offset = 0;
    file->ctxdata = nullptr;

    atomic_store(&file->refcount, 1, ATOMIC_RELAXED);

    int retval = vnode_open(node, file);

    if(retval < 0 && retval != -ENOSYS) {
        slab_free(file);
        vnode_decref(node);
        return retval;
    }

    if((flags & O_TRUNC) && node->type == VNODE_FILE) {
        vnode_attr_t attr = {};
        attr.mask = VATTR_SIZE;
        attr.size = 0;
        retval = vnode_remove_privs(node, credentials);

        if(retval == 0) {
            retval = vnode_set_attr(node, &attr);
        }

        if(retval < 0) {
            vnode_close(node, file);
            slab_free(file);
            vnode_decref(node);
            return retval;
        }
    }

    *out = file;

    return 0;
}

int file_open(vnode_t* restrict base_dir, const char* restrict path, unsigned flags, mode_t mode, const credentials_t* restrict credentials,
    file_t** restrict out)
{
    if(path == nullptr) {
        return -EINVAL;
    }

    // The name itself must be new, so a
    // symlink there counts even when it dangles
    bool exclusive_create = (flags & O_CREAT) && (flags & O_EXCL);

    unsigned resolve_flags = 0;

    if(exclusive_create || (flags & O_NOFOLLOW)) {
        resolve_flags = PATH_NOFOLLOW;
    }

    vnode_t* node;
    int retval = path_resolve_at(base_dir, path, resolve_flags, credentials, &node);
    bool created = false;

    if(retval == 0 && exclusive_create) {
        vnode_decref(node);
        return -EEXIST;
    }

    if(retval == -ENOENT && (flags & O_CREAT)) {
        const char* filename;
        vnode_t* parent;

        retval = path_resolve_parent_at(base_dir, path, credentials, &parent, &filename);

        if(retval < 0) {
            return retval;
        }

        retval = vnode_access(parent, credentials, W_OK | X_OK);

        if(retval == 0) {
            retval = vnode_create(parent, filename, VNODE_FILE, mode, credentials, &node);
            created = retval == 0;
        }

        vnode_decref(parent);

        if(retval == -EEXIST && !exclusive_create) {
            retval = path_resolve_at(base_dir, path, resolve_flags, credentials, &node); // Lost a race with another creator
        }

        if(retval < 0) {
            return retval;
        }
    }
    else if(retval < 0) {
        return retval;
    }

    // The creator gets the access it asked for
    // even when the new mode itself denies it
    if(!created) {
        retval = vnode_access(node, credentials, access_for(flags));

        if(retval < 0) {
            vnode_decref(node);
            return retval;
        }
    }

    return file_open_node(node, flags, credentials, out);
}

file_t* file_dupe(file_t* restrict file)
{
    if(file == nullptr) {
        return nullptr;
    }

    atomic_fetch_add(&file->refcount, 1, ATOMIC_RELAXED);

    return file;
}

void file_close(file_t* restrict file)
{
    if(file == nullptr) {
        return;
    }

    if(1 == atomic_fetch_sub(&file->refcount, 1, ATOMIC_ACQ_REL)) {
        vnode_close(file->node, file);
        vnode_decref(file->node);
        slab_free(file);
    }
}

ssize_t file_read(file_t* restrict file, void* restrict buffer, size_t size)
{
    ssize_t result;

    if(file == nullptr || buffer == nullptr) {
        return -EINVAL;
    }

    if(0 == (file->flags & O_RDONLY)) {
        return -EBADF;
    }

    result = vnode_read(file->node, file, buffer, size, file->offset);

    if(result > 0) {
        file->offset += result;
    }

    return result;
}

ssize_t file_write(file_t* restrict file, const credentials_t* restrict credentials, const void* restrict buffer, size_t size)
{
    if(file == nullptr || buffer == nullptr) {
        return -EINVAL;
    }

    if(0 == (file->flags & O_WRONLY)) {
        return -EBADF;
    }

    if(file->flags & O_APPEND) {
        stat_t stat;
        int retval = vnode_stat(file->node, &stat);

        if(retval < 0) {
            return retval;
        }

        if(stat.st_size < 0) {
            return -EINVAL;
        }

        file->offset = stat.st_size;
    }

    if(size > 0) {
        int retval = vnode_remove_privs(file->node, credentials);

        if(retval < 0) {
            return retval;
        }
    }

    ssize_t result = vnode_write(file->node, file, buffer, size, file->offset);

    if(result > 0) {
        file->offset += result;
    }

    return result;
}

int file_truncate(file_t* restrict file, const credentials_t* restrict credentials, off_t size)
{
    if(file == nullptr) {
        return -EINVAL;
    }

    if(0 == (file->flags & O_WRONLY)) {
        return -EBADF;
    }

    int retval = vnode_remove_privs(file->node, credentials);

    if(retval < 0) {
        return retval;
    }

    vnode_attr_t attr = {};
    attr.mask = VATTR_SIZE;
    attr.size = size;
    return vnode_set_attr(file->node, &attr);
}

int file_seek(file_t* restrict file, off_t offset, int origin)
{
    if(file == nullptr) {
        return -EINVAL;
    }

    off_t moved = vnode_seek(file->node, file, offset, origin);

    if(moved >= 0) {
        file->offset = moved;
        return 0;
    }

    if(moved != -ENOSYS) {
        return (int)(moved);
    }

    off_t new_offset;

    if(origin == SEEK_SET) {
        new_offset = offset;
        goto apply_offset;
    }

    if(origin == SEEK_CUR) {
        new_offset = offset + file->offset;
        goto apply_offset;
    }

    if(origin == SEEK_END) {
        stat_t stat;
        int retval = vnode_stat(file->node, &stat);

        if(retval < 0) {
            return retval;
        }

        new_offset = stat.st_size + offset;
        goto apply_offset;
    }

    return -EINVAL;

apply_offset:
    if(new_offset < 0)
        return -EINVAL;
    file->offset = new_offset;
    return 0;
}

int file_ioctl(file_t* restrict file, uint32_t request, uintptr_t argument)
{
    if(file == nullptr)
        return -EINVAL;
    return vnode_ioctl(file->node, file, request, argument);
}
