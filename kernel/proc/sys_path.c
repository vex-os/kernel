#include <vex/atomic.h>
#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/path.h>
#include <vex/process.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/uaccess.h>
#include <vex/vnode.h>

#define DIRENT_ALIGNMENT    8
#define GETDENTS_CHUNK_SIZE 1024

typedef struct getdents_state_s getdents_state_t;

struct getdents_state_s {
    char* buffer;
    size_t size;
    size_t filled;
    bool full;
};

static int resolve_rename_parent(int dirfd, const char* restrict path, const credentials_t* restrict credentials,
    vnode_t** restrict out_parent, const char** restrict out_name)
{
    vnode_t* base_dir;
    int retval = syscall_dirfd(dirfd, path, &base_dir);

    if(retval < 0) {
        return retval;
    }

    retval = path_resolve_parent_at(base_dir, path, credentials, out_parent, out_name);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    if(retval < 0) {
        return retval;
    }

    retval = vnode_may_delete(*out_parent, *out_name, credentials);

    if(retval < 0) {
        vnode_decref(*out_parent);
        return retval;
    }

    return 0;
}

static int resolve_new_parent(int dirfd, const char* restrict path, const credentials_t* restrict credentials,
    vnode_t** restrict out_parent, const char** restrict out_name)
{
    vnode_t* base_dir;
    int retval = syscall_dirfd(dirfd, path, &base_dir);

    if(retval < 0) {
        return retval;
    }

    retval = path_resolve_parent_at(base_dir, path, credentials, out_parent, out_name);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    if(retval < 0) {
        return retval;
    }

    retval = vnode_access(*out_parent, credentials, W_OK | X_OK);

    if(retval < 0) {
        vnode_decref(*out_parent);
        return retval;
    }

    return 0;
}

static size_t dirent_record_size(const dirent_t* restrict entry, size_t* restrict out_used)
{
    size_t used = offsetof(dirent_t, d_name) + strlen((const char*)(entry->d_name)) + 1;
    *out_used = used;
    return (used + DIRENT_ALIGNMENT - 1) & ~(size_t)(DIRENT_ALIGNMENT - 1);
}

static bool getdents_emit(dir_context_t* restrict dir_context, const dirent_t* restrict entry)
{
    getdents_state_t* state = dir_context->state;
    size_t used;
    size_t record_size = dirent_record_size(entry, &used);

    if(record_size > state->size - state->filled) {
        state->full = true;
        return false;
    }

    dirent_t* record = (dirent_t*)(state->buffer + state->filled);

    memcpy(record, entry, used);
    memset((char*)(record) + used, 0, record_size - used);
    record->d_reclen = record_size;
    state->filled += record_size;

    return true;
}

static intptr_t getdents_file(file_t* restrict file, const uintptr_t* restrict args)
{
    alignas(DIRENT_ALIGNMENT) char chunk[GETDENTS_CHUNK_SIZE];
    getdents_state_t state = {};
    dir_context_t dir_context = {};

    state.buffer = chunk;
    state.size = args[2];

    if(state.size > sizeof(chunk)) {
        state.size = sizeof(chunk);
    }

    dir_context.position = file->offset;
    dir_context.emit = &getdents_emit;
    dir_context.state = &state;

    int retval = vnode_iterate(file->node, &dir_context);

    if(retval < 0) {
        return retval;
    }

    if(state.filled == 0 && state.full) {
        return -EINVAL;
    }

    ssize_t copied = user_copyout(args[1], chunk, state.filled);

    if(copied < 0) {
        return copied;
    }

    file->offset = dir_context.position;

    return state.filled;
}

static intptr_t sys_fstatat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    unsigned flags = args[3];

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[1], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(args[0], path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    stat_t st;
    retval = vnode_stat(node, &st);

    vnode_decref(node);

    if(retval < 0) {
        return retval;
    }

    copied = user_copyout(args[2], &st, sizeof(st));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall(sys_fstatat, SYS_FSTATAT);

static intptr_t sys_fchmodat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    unsigned flags = args[3];

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[1], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(args[0], path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_chmod(node, thread->credentials, args[2]);

    vnode_decref(node);

    return retval;
}

define_syscall(sys_fchmodat, SYS_FCHMODAT);

static intptr_t sys_fchownat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    unsigned flags = args[4];

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[1], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(args[0], path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_chown(node, thread->credentials, args[2], args[3]);

    vnode_decref(node);

    return retval;
}

define_syscall(sys_fchownat, SYS_FCHOWNAT);

static intptr_t sys_utimensat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    unsigned flags = args[3];

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[1], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    timespec_t times[2];
    const timespec_t* times_ptr = nullptr;

    if(args[2]) {
        copied = user_copyin(times, args[2], sizeof(times));

        if(copied < 0) {
            return copied;
        }

        times_ptr = times;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(args[0], path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_utimes(node, thread->credentials, times_ptr);

    vnode_decref(node);

    return retval;
}

define_syscall(sys_utimensat, SYS_UTIMENSAT);

static intptr_t sys_mkdirat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[1], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    vnode_t* base_dir;
    int retval = syscall_dirfd(args[0], path, &base_dir);

    if(retval < 0) {
        return retval;
    }

    const credentials_t* credentials = thread->credentials;
    mode_t umask = atomic_load_n(&process->umask, ATOMIC_RELAXED);

    const char* name;
    vnode_t* parent;
    retval = path_resolve_parent_at(base_dir, path, credentials, &parent, &name);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    if(retval < 0) {
        return retval;
    }

    vnode_t* created;
    retval = vnode_access(parent, credentials, W_OK | X_OK);

    if(retval == 0) {
        retval = vnode_create(parent, name, VNODE_DIRECTORY, args[2] & ~umask, credentials, &created);
    }

    vnode_decref(parent);

    if(retval < 0) {
        return retval;
    }

    vnode_decref(created);

    return 0;
}

define_syscall(sys_mkdirat, SYS_MKDIRAT);

static intptr_t sys_unlinkat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    unsigned flags = args[2];

    if(flags & ~AT_REMOVEDIR) {
        return -EINVAL;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[1], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    vnode_t* base_dir;
    int retval = syscall_dirfd(args[0], path, &base_dir);

    if(retval < 0) {
        return retval;
    }

    const credentials_t* credentials = thread->credentials;

    const char* name;
    vnode_t* parent;
    retval = path_resolve_parent_at(base_dir, path, credentials, &parent, &name);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    if(retval < 0) {
        return retval;
    }

    retval = vnode_may_delete(parent, name, credentials);

    if(retval == 0) {
        retval = vnode_unlink(parent, name, flags & AT_REMOVEDIR);
    }

    vnode_decref(parent);

    return retval;
}

define_syscall(sys_unlinkat, SYS_UNLINKAT);

static intptr_t sys_renameat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char oldpath[PATH_MAX];
    ssize_t copied = user_copyin_str(oldpath, args[1], sizeof(oldpath));

    if(copied < 0) {
        return copied;
    }

    char newpath[PATH_MAX];
    copied = user_copyin_str(newpath, args[3], sizeof(newpath));

    if(copied < 0) {
        return copied;
    }

    const credentials_t* credentials = thread->credentials;

    const char* oldname;
    vnode_t* olddir;
    int retval = resolve_rename_parent(args[0], oldpath, credentials, &olddir, &oldname);

    if(retval < 0) {
        return retval;
    }

    const char* newname;
    vnode_t* newdir;
    retval = resolve_rename_parent(args[2], newpath, credentials, &newdir, &newname);

    if(retval < 0) {
        vnode_decref(olddir);
        return retval;
    }

    retval = vnode_rename(olddir, oldname, newdir, newname);

    vnode_decref(newdir);
    vnode_decref(olddir);

    return retval;
}

define_syscall(sys_renameat, SYS_RENAMEAT);

static intptr_t sys_linkat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    unsigned flags = args[4];

    if(flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    char oldpath[PATH_MAX];
    ssize_t copied = user_copyin_str(oldpath, args[1], sizeof(oldpath));

    if(copied < 0) {
        return copied;
    }

    char newpath[PATH_MAX];
    copied = user_copyin_str(newpath, args[3], sizeof(newpath));

    if(copied < 0) {
        return copied;
    }

    // Unlike most calls, a symlink is
    // linked to itself unless asked otherwise
    unsigned resolve_flags = flags & AT_EMPTY_PATH;

    if(0 == (flags & AT_SYMLINK_FOLLOW)) {
        resolve_flags |= AT_SYMLINK_NOFOLLOW;
    }

    vnode_t* target;
    int retval = syscall_resolve_at(args[0], oldpath, resolve_flags, &target);

    if(retval < 0) {
        return retval;
    }

    const char* name;
    vnode_t* parent;
    retval = resolve_new_parent(args[2], newpath, thread->credentials, &parent, &name);

    if(retval == 0) {
        retval = vnode_link(parent, name, target);
        vnode_decref(parent);
    }

    vnode_decref(target);

    return retval;
}

define_syscall(sys_linkat, SYS_LINKAT);

static intptr_t sys_symlinkat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char target[PATH_MAX];
    ssize_t copied = user_copyin_str(target, args[0], sizeof(target));

    if(copied < 0) {
        return copied;
    }

    char path[PATH_MAX];
    copied = user_copyin_str(path, args[2], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    const char* name;
    vnode_t* parent;
    int retval = resolve_new_parent(args[1], path, thread->credentials, &parent, &name);

    if(retval < 0) {
        return retval;
    }

    vnode_t* created;
    retval = vnode_symlink(parent, name, target, thread->credentials, &created);

    vnode_decref(parent);

    if(retval < 0) {
        return retval;
    }

    vnode_decref(created);

    return 0;
}

define_syscall(sys_symlinkat, SYS_SYMLINKAT);

static intptr_t sys_readlinkat(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[1], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(args[0], path, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH, &node);

    if(retval < 0) {
        return retval;
    }

    char target[PATH_MAX];
    size_t size = args[3];
    ssize_t length = -EINVAL;

    if(size > sizeof(target)) {
        size = sizeof(target);
    }

    if(node->type == VNODE_SYM_LINK) {
        length = vnode_readlink(node, target, size);
    }

    vnode_decref(node);

    if(length < 0) {
        return length;
    }

    copied = user_copyout(args[2], target, length);

    if(copied < 0) {
        return copied;
    }

    return length;
}

define_syscall(sys_readlinkat, SYS_READLINKAT);

static intptr_t sys_getdents(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &getdents_file);
}

define_syscall(sys_getdents, SYS_GETDENTS);

static intptr_t sys_getcwd(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    vnode_t* cwd = process_get_cwd(process);

    if(cwd == nullptr) {
        return -ENOENT;
    }

    char path[PATH_MAX];
    ssize_t length = path_build(cwd, path, sizeof(path));

    vnode_decref(cwd);

    if(length == -ERANGE) {
        return -ENAMETOOLONG;
    }

    if(length < 0) {
        return length;
    }

    if((size_t)(length) > args[1]) {
        return -ERANGE;
    }

    ssize_t copied = user_copyout(args[0], path, length);

    if(copied < 0) {
        return copied;
    }

    return length;
}

define_syscall(sys_getcwd, SYS_GETCWD);

static intptr_t sys_chdir(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, args[0], sizeof(path));

    if(copied < 0) {
        return copied;
    }

    vnode_t* base_dir;
    int retval = syscall_dirfd(AT_FDCWD, path, &base_dir);

    if(retval < 0) {
        return retval;
    }

    const credentials_t* credentials = thread->credentials;

    vnode_t* node;
    retval = path_resolve_at(base_dir, path, 0, credentials, &node);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    if(retval < 0) {
        return retval;
    }

    if(node->type != VNODE_DIRECTORY) {
        vnode_decref(node);
        return -ENOTDIR;
    }

    retval = vnode_access(node, credentials, X_OK);

    if(retval < 0) {
        vnode_decref(node);
        return retval;
    }

    process_set_cwd(process, node);

    return 0;
}

define_syscall(sys_chdir, SYS_CHDIR);

static intptr_t sys_fchdir(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    int fd = args[0];

    if(fd == AT_FDCWD) {
        return -EBADF;
    }

    // An empty path is never absolute, so this
    // always hands back the directory behind fd
    vnode_t* node;
    int retval = syscall_dirfd(fd, "", &node);

    if(retval < 0) {
        return retval;
    }

    const credentials_t* credentials = thread->credentials;
    retval = vnode_access(node, credentials, X_OK);

    if(retval < 0) {
        vnode_decref(node);
        return retval;
    }

    process_set_cwd(process, node);

    return 0;
}

define_syscall(sys_fchdir, SYS_FCHDIR);
