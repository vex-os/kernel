#include <vex/assert.h>
#include <vex/fcntl.h>
#include <vex/mount.h>
#include <vex/path.h>
#include <vex/pslab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/vnode.h>

#define MAX_SYMLINK_DEPTH 8

static int resolve_internal(vnode_t* restrict base_dir, const char* restrict path, unsigned flags,
    const credentials_t* restrict credentials, vnode_t** restrict out, unsigned depth);

static int follow_symlink(vnode_t* restrict node, vnode_t* restrict link, const char* restrict rest, unsigned flags,
    const credentials_t* restrict credentials, vnode_t** restrict out, unsigned depth)
{
    if(depth > MAX_SYMLINK_DEPTH) {
        return -ELOOP;
    }

    char* target = pslab_alloc_virt();

    if(target == nullptr) {
        return -ENOMEM;
    }

    ssize_t length = vnode_readlink(link, target, PATH_LIM);

    if(length < 0) {
        pslab_free_virt(target);
        return length;
    }

    if(length > PATH_LIM) {
        pslab_free_virt(target);
        return -ENAMETOOLONG;
    }

    target[length] = 0;

    vnode_t* base = nullptr;
    int retval;

    if(target[0] == PATH_SEPARATOR) {
        base = vfs_get_root();

        if(base == nullptr) {
            pslab_free_virt(target);
            return -ENOENT;
        }
    }
    else {
        base = node;

        vnode_incref(base);
    }

    vnode_t* resolved = nullptr;
    bool has_rest = rest && *rest;
    unsigned target_flags = flags;

    if(has_rest) {
        target_flags = 0;
    }

    if(target[0] == PATH_SEPARATOR) {
        retval = resolve_internal(base, &target[1], target_flags, credentials, &resolved, depth + 1);
    }
    else {
        retval = resolve_internal(base, target, target_flags, credentials, &resolved, depth + 1);
    }

    vnode_decref(base);

    pslab_free_virt(target);

    if(retval < 0) {
        return retval;
    }

    if(has_rest) {
        retval = resolve_internal(resolved, rest, flags, credentials, out, depth + 1);
        vnode_decref(resolved);
        return retval;
    }

    *out = resolved;

    return 0;
}

// The root of a mounted filesystem stands in for the directory it covers, so
// its parent is the parent of that directory; stacked mounts climb several times
static vnode_t* parent_of(vnode_t* restrict dir)
{
    while(dir->fs && dir->fs->root == dir && dir->fs->mountpoint) {
        dir = dir->fs->mountpoint;
    }

    spinlock_acquire(&dir->lock);

    vnode_t* parent = dir->parent;

    if(parent == nullptr) {
        parent = dir;
    }

    vnode_incref(parent);

    spinlock_release(&dir->lock);

    return parent;
}

static int resolve_internal(vnode_t* restrict base_dir, const char* restrict path, unsigned flags,
    const credentials_t* restrict credentials, vnode_t** restrict out, unsigned depth)
{
    char component[NAME_SIZE];

    kassert(base_dir);
    kassert(path);
    kassert(out);

    const char* chrptr = path;
    vnode_t* current = base_dir;

    vnode_incref(current);

    while(*chrptr) {
        while(*chrptr == PATH_SEPARATOR) {
            chrptr += 1;
        }

        if(*chrptr == 0) {
            break;
        }

        const char* sep = strchr(chrptr, PATH_SEPARATOR);

        ptrdiff_t length;

        if(sep == nullptr) {
            length = strlen(chrptr);
        }
        else {
            length = sep - chrptr;
        }

        if(length > NAME_MAX) {
            vnode_decref(current);
            return -ENOTDIR;
        }

        memcpy(component, chrptr, length);
        component[length] = 0;
        chrptr += length;

        if(current->type != VNODE_DIRECTORY) {
            vnode_decref(current);
            return -ENOTDIR;
        }

        int retval = vnode_access(current, credentials, X_OK);

        if(retval < 0) {
            vnode_decref(current);
            return retval;
        }

        if(component[0] == '.' && component[1] == 0) {
            continue;
        }

        if(component[0] == '.' && component[1] == '.' && component[2] == 0) {
            vnode_t* upper = parent_of(current);

            vnode_decref(current);
            current = upper;
            continue;
        }

        vnode_t* next = nullptr;
        retval = vnode_lookup(current, component, &next);

        if(retval < 0) {
            vnode_decref(current);
            return retval;
        }

        // A trailing separator still counts as more path, as POSIX
        // requires "link/" to resolve to the directory behind it
        bool is_last = *chrptr == 0;

        if(next->type == VNODE_SYM_LINK && (!is_last || 0 == (flags & PATH_NOFOLLOW))) {
            retval = follow_symlink(current, next, chrptr, flags, credentials, out, depth);
            vnode_decref(current);
            vnode_decref(next);
            return retval;
        }

        vnode_decref(current);
        current = next;
    }

    *out = current;

    return 0;
}

int path_resolve(const char* restrict path, const credentials_t* restrict credentials, vnode_t** restrict out)
{
    kassert(path);
    kassert(credentials);
    kassert(out);

    if(path[0] != PATH_SEPARATOR) {
        return -EINVAL;
    }

    return path_resolve_at(nullptr, path, 0, credentials, out);
}

int path_resolve_at(vnode_t* restrict base_dir, const char* restrict path, unsigned flags, const credentials_t* restrict credentials,
    vnode_t** restrict out)
{
    kassert(path);
    kassert(credentials);
    kassert(out);

    if(path[0] == 0) {
        return -ENOENT;
    }

    if(base_dir && path[0] != PATH_SEPARATOR) {
        return resolve_internal(base_dir, path, flags, credentials, out, 0);
    }

    vnode_t* root = vfs_get_root();

    if(root == nullptr) {
        return -ENOENT;
    }

    while(path[0] == PATH_SEPARATOR) {
        path += 1;
    }

    int retval = resolve_internal(root, path, flags, credentials, out, 0);

    vnode_decref(root);

    return retval;
}

int path_resolve_parent_at(vnode_t* restrict base_dir, const char* restrict path, const credentials_t* restrict credentials,
    vnode_t** restrict out_parent, const char** restrict out_name)
{
    kassert(path);
    kassert(credentials);
    kassert(out_parent);
    kassert(out_name);

    if(path[0] == 0) {
        return -ENOENT;
    }

    const char* slash = strrchr(path, PATH_SEPARATOR);

    out_name[0] = slash ? &slash[1] : path;

    int retval;

    if(slash == nullptr) {
        retval = path_resolve_at(base_dir, ".", 0, credentials, out_parent);
    }
    else if(slash == path) {
        retval = path_resolve_at(nullptr, "/", 0, credentials, out_parent);
    }
    else {
        size_t dir_length = slash - path;

        if(dir_length > PATH_LIM) {
            return -ENAMETOOLONG;
        }

        char* dir_path = pslab_alloc_virt();

        if(dir_path == nullptr) {
            return -ENOMEM;
        }

        memcpy(dir_path, path, dir_length);

        dir_path[dir_length] = 0x00;

        retval = path_resolve_at(base_dir, dir_path, 0, credentials, out_parent);

        pslab_free_virt(dir_path);
    }

    if(retval < 0) {
        return retval;
    }

    if(out_parent[0]->type != VNODE_DIRECTORY) {
        vnode_decref(out_parent[0]);
        return -ENOTDIR;
    }

    return 0;
}
