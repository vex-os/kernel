#include <vex/atomic.h>
#include <vex/cred.h>
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

#include "syscall_internal.h"

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

static intptr_t sys_getdents(file_t* restrict file, uintptr_t user_buffer, size_t size)
{
    alignas(DIRENT_ALIGNMENT) char chunk[GETDENTS_CHUNK_SIZE];
    getdents_state_t state = {};
    dir_context_t dir_context = {};

    state.buffer = chunk;
    state.size = size;

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

    ssize_t copied = user_copyout(user_buffer, chunk, state.filled);

    if(copied < 0) {
        return copied;
    }

    file->offset = dir_context.position;

    return state.filled;
}

define_syscall_fd(sys_getdents, SYS_GETDENTS, 3);

static intptr_t sys_fstatat(int dirfd, const char* restrict path, uintptr_t user_stat, unsigned flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(dirfd, path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    stat_t st;
    retval = vnode_stat(node, &st);

    vnode_decref(node);

    if(retval < 0) {
        return retval;
    }

    ssize_t copied = user_copyout(user_stat, &st, sizeof(st));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall_dirfd(sys_fstatat, SYS_FSTATAT, 4);

static intptr_t sys_faccessat(int dirfd, const char* restrict path, unsigned mode, unsigned flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(mode & ~(R_OK | W_OK | X_OK)) {
        return -EINVAL;
    }

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_EACCESS)) {
        return -EINVAL;
    }

    // The walk to the node checks the search rights of the directories with the effective ids
    vnode_t* node;
    int retval = syscall_resolve_at(dirfd, path, flags & (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH), &node);

    if(retval < 0) {
        return retval;
    }

    const credentials_t* credentials = thread->credentials;
    credentials_t* real = nullptr;

    if(0 == (flags & AT_EACCESS)) {
        real = cred_copy_real(credentials);

        if(real == nullptr) {
            vnode_decref(node);
            return -ENOMEM;
        }

        credentials = real;
    }

    retval = vnode_access(node, credentials, mode);

    if(real) {
        cred_decref(real);
    }

    vnode_decref(node);

    return retval;
}

define_syscall_dirfd(sys_faccessat, SYS_FACCESSAT, 4);

static intptr_t sys_fchmodat(int dirfd, const char* restrict path, mode_t mode, unsigned flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(dirfd, path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_chmod(node, thread->credentials, mode);

    vnode_decref(node);

    return retval;
}

define_syscall_dirfd(sys_fchmodat, SYS_FCHMODAT, 4);

static intptr_t sys_fchownat(int dirfd, const char* restrict path, uid_t uid, gid_t gid, unsigned flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(dirfd, path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_chown(node, thread->credentials, uid, gid);

    vnode_decref(node);

    return retval;
}

define_syscall_dirfd(sys_fchownat, SYS_FCHOWNAT, 5);

static intptr_t sys_utimensat(int dirfd, const char* restrict path, uintptr_t user_times, unsigned flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    timespec_t times[2];
    const timespec_t* times_ptr = nullptr;

    if(user_times) {
        ssize_t copied = user_copyin(times, user_times, sizeof(times));

        if(copied < 0) {
            return copied;
        }

        times_ptr = times;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(dirfd, path, flags, &node);

    if(retval < 0) {
        return retval;
    }

    retval = vnode_utimes(node, thread->credentials, times_ptr);

    vnode_decref(node);

    return retval;
}

define_syscall_dirfd(sys_utimensat, SYS_UTIMENSAT, 4);

static intptr_t sys_mkdirat(int dirfd, const char* restrict path, mode_t mode)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    vnode_t* base_dir;
    int retval = syscall_dirfd(dirfd, path, &base_dir);

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
        retval = vnode_create(parent, name, VNODE_DIRECTORY, mode & ~umask, credentials, &created);
    }

    vnode_decref(parent);

    if(retval < 0) {
        return retval;
    }

    vnode_decref(created);

    return 0;
}

define_syscall_dirfd(sys_mkdirat, SYS_MKDIRAT, 3);

static intptr_t sys_unlinkat(int dirfd, const char* restrict path, unsigned flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~AT_REMOVEDIR) {
        return -EINVAL;
    }

    vnode_t* base_dir;
    int retval = syscall_dirfd(dirfd, path, &base_dir);

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

define_syscall_dirfd(sys_unlinkat, SYS_UNLINKAT, 3);

static intptr_t sys_renameat(int old_dirfd, uintptr_t user_old_path, int new_dirfd, uintptr_t user_new_path)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char oldpath[PATH_MAX];
    ssize_t copied = user_copyin_str(oldpath, user_old_path, sizeof(oldpath));

    if(copied < 0) {
        return copied;
    }

    char newpath[PATH_MAX];
    copied = user_copyin_str(newpath, user_new_path, sizeof(newpath));

    if(copied < 0) {
        return copied;
    }

    const credentials_t* credentials = thread->credentials;

    const char* oldname;
    vnode_t* olddir;
    int retval = resolve_rename_parent(old_dirfd, oldpath, credentials, &olddir, &oldname);

    if(retval < 0) {
        return retval;
    }

    const char* newname;
    vnode_t* newdir;
    retval = resolve_rename_parent(new_dirfd, newpath, credentials, &newdir, &newname);

    if(retval < 0) {
        vnode_decref(olddir);
        return retval;
    }

    retval = vnode_rename(olddir, oldname, newdir, newname);

    vnode_decref(newdir);
    vnode_decref(olddir);

    return retval;
}

define_syscall(sys_renameat, SYS_RENAMEAT, 4);

static intptr_t sys_linkat(int old_dirfd, uintptr_t user_old_path, int new_dirfd, uintptr_t user_new_path, unsigned flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }

    char oldpath[PATH_MAX];
    ssize_t copied = user_copyin_str(oldpath, user_old_path, sizeof(oldpath));

    if(copied < 0) {
        return copied;
    }

    char newpath[PATH_MAX];
    copied = user_copyin_str(newpath, user_new_path, sizeof(newpath));

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
    int retval = syscall_resolve_at(old_dirfd, oldpath, resolve_flags, &target);

    if(retval < 0) {
        return retval;
    }

    const char* name;
    vnode_t* parent;
    retval = resolve_new_parent(new_dirfd, newpath, thread->credentials, &parent, &name);

    if(retval == 0) {
        retval = vnode_link(parent, name, target);
        vnode_decref(parent);
    }

    vnode_decref(target);

    return retval;
}

define_syscall(sys_linkat, SYS_LINKAT, 5);

static intptr_t sys_symlinkat(uintptr_t user_target, int dirfd, uintptr_t user_path)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char target[PATH_MAX];
    ssize_t copied = user_copyin_str(target, user_target, sizeof(target));

    if(copied < 0) {
        return copied;
    }

    char path[PATH_MAX];
    copied = user_copyin_str(path, user_path, sizeof(path));

    if(copied < 0) {
        return copied;
    }

    const char* name;
    vnode_t* parent;
    int retval = resolve_new_parent(dirfd, path, thread->credentials, &parent, &name);

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

define_syscall(sys_symlinkat, SYS_SYMLINKAT, 3);

static intptr_t sys_readlinkat(int dirfd, const char* restrict path, uintptr_t user_buffer, size_t size)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    vnode_t* node;
    int retval = syscall_resolve_at(dirfd, path, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH, &node);

    if(retval < 0) {
        return retval;
    }

    char target[PATH_MAX];
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

    ssize_t copied = user_copyout(user_buffer, target, length);

    if(copied < 0) {
        return copied;
    }

    return length;
}

define_syscall_dirfd(sys_readlinkat, SYS_READLINKAT, 4);

static intptr_t sys_getcwd(uintptr_t user_buffer, size_t size)
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

    if((size_t)(length) > size) {
        return -ERANGE;
    }

    ssize_t copied = user_copyout(user_buffer, path, length);

    if(copied < 0) {
        return copied;
    }

    return length;
}

define_syscall(sys_getcwd, SYS_GETCWD, 2);

static intptr_t sys_chdir(uintptr_t user_path)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    char path[PATH_MAX];
    ssize_t copied = user_copyin_str(path, user_path, sizeof(path));

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

define_syscall(sys_chdir, SYS_CHDIR, 1);

static intptr_t sys_fchdir(int fd)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

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

define_syscall(sys_fchdir, SYS_FCHDIR, 1);
