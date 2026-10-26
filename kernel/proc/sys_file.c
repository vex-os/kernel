#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/mount.h>
#include <vex/path.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/stat.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vnode.h>

#define FCNTL_SETTABLE_FLAGS (O_APPEND | O_NONBLOCK)

static_assert(FILE_IO_CHUNK <= PAGE_SIZE);

static char* io_page_of(thread_t* restrict thread)
{
    if(thread->io_page == nullptr) {
        thread->io_page = pslab_alloc_virt();
    }

    return thread->io_page;
}

static intptr_t read_file(file_t* restrict file, const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    uintptr_t user_buf = args[1];
    size_t remaining = args[2];
    size_t total = 0;

    char* buffer = io_page_of(thread);

    if(buffer == nullptr) {
        return -ENOMEM;
    }

    while(remaining > 0) {
        size_t chunk;

        if(remaining > FILE_IO_CHUNK) {
            chunk = FILE_IO_CHUNK;
        }
        else {
            chunk = remaining;
        }

        ssize_t num_read = file_read(file, buffer, chunk);

        if(num_read < 0) {
            if(total > 0)
                return total;
            return num_read;
        }

        if(num_read == 0) {
            break;
        }

        ssize_t copied = user_copyout(user_buf, buffer, num_read);

        if(copied < 0) {
            return copied;
        }

        user_buf += num_read;
        total += num_read;
        remaining -= num_read;

        // A device such as a tty hands out what it has at once,
        // and asking it again would wait for more to come in
        if((size_t)(num_read) < chunk || file->node->type == VNODE_CHAR_DEV) {
            break;
        }
    }

    return total;
}

static intptr_t write_file(file_t* restrict file, const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    uintptr_t user_buf = args[1];
    size_t remaining = args[2];
    size_t total = 0;

    char* buffer = io_page_of(thread);

    if(buffer == nullptr) {
        return -ENOMEM;
    }

    while(remaining > 0) {
        size_t chunk;

        if(remaining > FILE_IO_CHUNK) {
            chunk = FILE_IO_CHUNK;
        }
        else {
            chunk = remaining;
        }

        ssize_t copied = user_copyin(buffer, user_buf, chunk);

        if(copied < 0) {
            return copied;
        }

        ssize_t num_written = file_write(file, thread->credentials, buffer, chunk);

        if(num_written < 0) {
            if(total > 0)
                return total;
            return num_written;
        }

        if(num_written == 0) {
            break;
        }

        user_buf += num_written;
        total += num_written;
        remaining -= num_written;

        // Only a non-blocking device takes less than it is given
        if((size_t)(num_written) < chunk) {
            break;
        }
    }

    return total;
}

static intptr_t seek_file(file_t* restrict file, const uintptr_t* restrict args)
{
    return file_seek(file, args[1], args[2]);
}

static intptr_t ftruncate_file(file_t* restrict file, const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();

    return file_truncate(file, thread->credentials, args[1]);
}

static intptr_t fstat_file(file_t* restrict file, const uintptr_t* restrict args)
{
    stat_t st;
    int retval = vnode_stat(file->node, &st);

    if(retval < 0) {
        return retval;
    }

    ssize_t copied = user_copyout(args[1], &st, sizeof(st));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

static intptr_t ioctl_file(file_t* restrict file, const uintptr_t* restrict args)
{
    return file_ioctl(file, args[1], args[2]);
}

static intptr_t fsync_file(file_t* restrict file, const uintptr_t* restrict args)
{
    vnode_t* node;
    int retval = vfs_cross_mount(file->node, &node);

    if(retval < 0) {
        return retval;
    }

    vfs_t* fs = node->fs;

    retval = vnode_fsync(node);

    if(retval == 0 && fs && fs->ops->sync) {
        retval = fs->ops->sync(fs);
    }

    vnode_decref(node);

    return retval;
}

static intptr_t fcntl_file(file_t* restrict file, const uintptr_t* restrict args)
{
    unsigned command = args[1];
    unsigned flags = file_flags(file);

    if(command == F_GETFL) {
        return flags;
    }

    if(command != F_SETFL) {
        return -EINVAL;
    }

    // As in POSIX, the access mode and the creation flags stay as open left them
    unsigned changed = args[2] & FCNTL_SETTABLE_FLAGS;

    while(!atomic_cmpxchg_weak(&file->flags, &flags, (flags & ~FCNTL_SETTABLE_FLAGS) | changed, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
        // empty
    }

    return 0;
}

static intptr_t sys_openat(const uintptr_t* restrict args)
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

    file_t* file;
    retval = file_open(base_dir, path, args[2], args[3] & ~umask, credentials, &file);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    if(retval < 0) {
        return retval;
    }

    int fd = process_fd_alloc(process, file);

    if(fd < 0) {
        file_close(file);
        return fd;
    }

    return fd;
}

define_syscall(sys_openat, SYS_OPENAT);

static intptr_t sys_close(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    return process_fd_release(process, args[0]);
}

define_syscall(sys_close, SYS_CLOSE);

static intptr_t sys_read(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &read_file);
}

define_syscall(sys_read, SYS_READ);

static intptr_t sys_write(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &write_file);
}

define_syscall(sys_write, SYS_WRITE);

static intptr_t sys_seek(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &seek_file);
}

define_syscall(sys_seek, SYS_SEEK);

static intptr_t sys_ioctl(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &ioctl_file);
}

define_syscall(sys_ioctl, SYS_IOCTL);

static intptr_t sys_dup(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    return process_fd_dup(process, args[0], -1);
}

define_syscall(sys_dup, SYS_DUP);

static intptr_t sys_dup2(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    int old_fd = args[0];
    int new_fd = args[1];

    if(new_fd < 0) {
        return -EBADF;
    }

    return process_fd_dup(process, old_fd, new_fd);
}

define_syscall(sys_dup2, SYS_DUP2);

static intptr_t sys_fstat(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &fstat_file);
}

define_syscall(sys_fstat, SYS_FSTAT);

static intptr_t sys_ftruncate(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &ftruncate_file);
}

define_syscall(sys_ftruncate, SYS_FTRUNCATE);

static intptr_t sys_fsync(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &fsync_file);
}

define_syscall(sys_fsync, SYS_FSYNC);

static intptr_t sys_fcntl(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &fcntl_file);
}

define_syscall(sys_fcntl, SYS_FCNTL);
