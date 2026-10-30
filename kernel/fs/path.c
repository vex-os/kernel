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

typedef struct name_search_s name_search_t;

struct name_search_s {
    ino_t ino;
    bool found;
    char name[NAME_SIZE];
};

static vnode_t* (*root_source)(void);

static int resolve_internal(vnode_t* restrict base_dir, vnode_t* root, const char* restrict path, unsigned flags,
    const credentials_t* restrict credentials, vnode_t** restrict out, unsigned depth);

// The root of the process, or of the VFS when the caller has none or no source is set
static vnode_t* current_root(void)
{
    if(root_source) {
        vnode_t* root = root_source();

        if(root) {
            return root;
        }
    }

    return vfs_get_root();
}

static int follow_symlink(vnode_t* restrict node, vnode_t* root, vnode_t* restrict link, const char* restrict rest, unsigned flags,
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
        base = root;
    }
    else {
        base = node;
    }

    vnode_incref(base);

    vnode_t* resolved = nullptr;
    bool has_rest = rest && *rest;
    unsigned target_flags = flags;

    if(has_rest) {
        target_flags = 0;
    }

    if(target[0] == PATH_SEPARATOR) {
        retval = resolve_internal(base, root, &target[1], target_flags, credentials, &resolved, depth + 1);
    }
    else {
        retval = resolve_internal(base, root, target, target_flags, credentials, &resolved, depth + 1);
    }

    vnode_decref(base);

    pslab_free_virt(target);

    if(retval < 0) {
        return retval;
    }

    if(has_rest) {
        retval = resolve_internal(resolved, root, rest, flags, credentials, out, depth + 1);
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

// The name of a mounted filesystem's root is the name of the directory it covers
static vnode_t* covered_dir(vnode_t* restrict dir)
{
    while(dir->fs && dir->fs->root == dir && dir->fs->mountpoint) {
        dir = dir->fs->mountpoint;
    }

    vnode_incref(dir);

    return dir;
}

static bool name_search_emit(dir_context_t* restrict dir_context, const dirent_t* restrict entry)
{
    name_search_t* search = dir_context->state;

    if(entry->d_ino != search->ino) {
        return true;
    }

    // A directory is also its own "." and its child's ".."
    bool dot = entry->d_name[0] == '.' && entry->d_name[1] == 0;
    bool dotdot = entry->d_name[0] == '.' && entry->d_name[1] == '.' && entry->d_name[2] == 0;

    if(dot || dotdot) {
        return true;
    }

    kstrncpy(search->name, (const char*)(entry->d_name), sizeof(search->name));
    search->found = true;

    return false;
}

static int resolve_internal(vnode_t* restrict base_dir, vnode_t* root, const char* restrict path, unsigned flags,
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
            // The root of the process is the top of its tree, whatever lies above it
            if(current == root) {
                continue;
            }

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
            retval = follow_symlink(current, root, next, chrptr, flags, credentials, out, depth);
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

    vnode_t* root = current_root();

    if(root == nullptr) {
        return -ENOENT;
    }

    int retval;

    if(base_dir && path[0] != PATH_SEPARATOR) {
        retval = resolve_internal(base_dir, root, path, flags, credentials, out, 0);
    }
    else {
        while(path[0] == PATH_SEPARATOR) {
            path += 1;
        }

        retval = resolve_internal(root, root, path, flags, credentials, out, 0);
    }

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

ssize_t path_build(vnode_t* restrict node, char* restrict buffer, size_t size)
{
    kassert(node);
    kassert(buffer);

    if(size == 0) {
        return -ERANGE;
    }

    vnode_t* root = current_root();

    if(root == nullptr) {
        return -ENOENT;
    }

    size_t end = size - 1;
    buffer[end] = 0;

    name_search_t search;
    vnode_t* current = covered_dir(node);
    int retval = 0;

    while(current != root) {
        vnode_t* parent = parent_of(current);

        if(parent == current) {
            vnode_decref(parent);
            retval = -ENOENT;
            break;
        }

        dir_context_t dir_context = {};

        search.ino = current->ino;
        search.found = false;
        dir_context.emit = &name_search_emit;
        dir_context.state = &search;

        retval = vnode_iterate(parent, &dir_context);

        if(retval == 0 && !search.found) {
            retval = -ENOENT;
        }

        size_t length = strlen(search.name);

        if(retval == 0 && length + 1 > end) {
            retval = -ERANGE;
        }

        if(retval < 0) {
            vnode_decref(parent);
            break;
        }

        end -= length;
        memcpy(&buffer[end], search.name, length);
        end -= 1;
        buffer[end] = PATH_SEPARATOR;

        vnode_decref(current);
        current = covered_dir(parent);
        vnode_decref(parent);
    }

    vnode_decref(current);
    vnode_decref(root);

    if(retval < 0) {
        return retval;
    }

    if(end == size - 1) {
        if(end == 0) {
            return -ERANGE;
        }

        end -= 1;
        buffer[end] = PATH_SEPARATOR;
    }

    size_t length = size - end;

    memmove(buffer, &buffer[end], length);

    return length;
}

void path_set_root_source(vnode_t* (*source)(void))
{
    root_source = source;
}
