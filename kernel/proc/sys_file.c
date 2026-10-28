#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/path.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/stat.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/uio.h>
#include <vex/vnode.h>

#define FCNTL_SETTABLE_FLAGS (O_APPEND | O_NONBLOCK)

static_assert(FILE_IO_CHUNK <= PAGE_SIZE);

typedef struct io_cursor_s io_cursor_t;

// Where the data of a read or write goes to or comes from: one buffer, or the buffers of an iovec array
// in user memory. The array is read one element at a time, as it can be larger than anything on the stack
struct io_cursor_s {
    uintptr_t iov; // Zero elements for a single buffer
    size_t count;
    size_t index;
    uintptr_t base; // What is left of the current buffer
    size_t left;
    size_t budget; // What the elements not read yet may add up to, so that an array changed meanwhile cannot overflow the total
    size_t size;   // All the buffers together
};

static char* io_page_of(thread_t* restrict thread)
{
    if(thread->io_page == nullptr) {
        thread->io_page = pslab_alloc_virt();
    }

    return thread->io_page;
}

static void io_cursor_init_buffer(io_cursor_t* restrict cursor, uintptr_t base, size_t size)
{
    cursor->iov = 0;
    cursor->count = 0;
    cursor->index = 0;
    cursor->base = base;
    cursor->left = size;
    cursor->budget = 0;
    cursor->size = size;
}

// The sum of the lengths has to fit in the result of the call, which is checked before any data moves
static int io_cursor_init_vector(io_cursor_t* restrict cursor, uintptr_t user_iov, size_t count)
{
    static_assert(sizeof(size_t) == sizeof(uint64_t));

    if(count > IOV_MAX) {
        return -EINVAL;
    }

    size_t size = 0;

    for(size_t i = 0; i < count; ++i) {
        iovec_t iov;
        ssize_t copied = user_copyin(&iov, user_iov + i * sizeof(iov), sizeof(iov));

        if(copied < 0) {
            return copied;
        }

        if(iov.len > (size_t)(INT64_MAX) - (size)) {
            return -EINVAL;
        }

        size += iov.len;
    }

    cursor->iov = user_iov;
    cursor->count = count;
    cursor->index = 0;
    cursor->base = 0;
    cursor->left = 0;
    cursor->budget = size;
    cursor->size = size;

    return 0;
}

// Moves on to the next non-empty buffer when the current one is used up
// @retval 1 There is data left
// @retval 0 All the buffers are used up
static int io_cursor_fill(io_cursor_t* restrict cursor)
{
    while(cursor->left == 0) {
        if(cursor->index >= cursor->count) {
            return 0;
        }

        iovec_t iov;
        ssize_t copied = user_copyin(&iov, cursor->iov + cursor->index * sizeof(iov), sizeof(iov));

        if(copied < 0) {
            return copied;
        }

        size_t length = iov.len;

        if(length > cursor->budget) {
            length = cursor->budget;
        }

        cursor->index += 1;
        cursor->budget -= length;
        cursor->base = iov.base;
        cursor->left = length;
    }

    return 1;
}

static void io_cursor_advance(io_cursor_t* restrict cursor, size_t size)
{
    cursor->base += size;
    cursor->left -= size;
}

// A device such as a tty hands out what it has at once,
// and asking it again would wait for more to come in
static bool hands_out_at_once(const file_t* restrict file)
{
    return file->node->type == VNODE_CHAR_DEV;
}

static intptr_t read_chunks(file_t* restrict file, io_cursor_t* restrict cursor, off_t* restrict position)
{
    thread_t* thread = this_thread();
    size_t total = 0;

    char* buffer = io_page_of(thread);

    if(buffer == nullptr) {
        return -ENOMEM;
    }

    for(;;) {
        int status = io_cursor_fill(cursor);

        if(status <= 0) {
            if(status < 0 && total == 0) {
                return status;
            }

            return total;
        }

        size_t chunk = cursor->left;

        if(chunk > FILE_IO_CHUNK) {
            chunk = FILE_IO_CHUNK;
        }

        ssize_t num_read = file_read(file, buffer, chunk, position);

        if(num_read < 0) {
            if(total > 0)
                return total;
            return num_read;
        }

        if(num_read == 0) {
            break;
        }

        ssize_t copied = user_copyout(cursor->base, buffer, num_read);

        if(copied < 0) {
            return copied;
        }

        io_cursor_advance(cursor, num_read);
        total += num_read;

        if((size_t)(num_read) < chunk || hands_out_at_once(file)) {
            break;
        }
    }

    return total;
}

// The offset of the file goes to its end before each chunk, so that appenders mix at chunk borders only
static int move_to_end(file_t* restrict file)
{
    stat_t stat;
    int retval = vnode_stat(file->node, &stat);

    if(retval < 0) {
        return retval;
    }

    if(stat.st_size < 0) {
        return -EINVAL;
    }

    file->offset = stat.st_size;

    return 0;
}

// Fills one chunk from the buffers, so that a call with up to a chunk of data in all reaches
// the file as one write. A fault keeps what came before it in the chunk
// @retval 1 The chunk is full
// @retval 0 All the buffers are used up
static int gather_chunk(io_cursor_t* restrict cursor, char* restrict buffer, size_t* restrict filled)
{
    *filled = 0;

    while(*filled < FILE_IO_CHUNK) {
        int status = io_cursor_fill(cursor);

        if(status <= 0) {
            return status;
        }

        size_t size = cursor->left;

        if(size > FILE_IO_CHUNK - *filled) {
            size = FILE_IO_CHUNK - *filled;
        }

        ssize_t copied = user_copyin(buffer + *filled, cursor->base, size);

        if(copied < 0) {
            return copied;
        }

        io_cursor_advance(cursor, size);
        *filled += size;
    }

    return 1;
}

static intptr_t write_chunks(file_t* restrict file, io_cursor_t* restrict cursor, off_t* restrict position, bool append)
{
    thread_t* thread = this_thread();
    size_t total = 0;

    char* buffer = io_page_of(thread);

    if(buffer == nullptr) {
        return -ENOMEM;
    }

    for(;;) {
        size_t filled;
        int status = gather_chunk(cursor, buffer, &filled);

        if(filled == 0) {
            if(status < 0 && total == 0) {
                return status;
            }

            return total;
        }

        if(append) {
            int retval = move_to_end(file);

            if(retval < 0) {
                if(total > 0)
                    return total;
                return retval;
            }
        }

        ssize_t num_written = file_write(file, thread->credentials, buffer, filled, position);

        if(num_written < 0) {
            if(total > 0)
                return total;
            return num_written;
        }

        if(num_written == 0) {
            break;
        }

        total += num_written;

        // Only a non-blocking device takes less than it is given
        if((size_t)(num_written) < filled || status <= 0) {
            break;
        }
    }

    return total;
}

// The whole range has to be a valid offset,
// so that a long call never wraps around
static bool range_valid(off_t offset, size_t size)
{
    static_assert(sizeof(off_t) == sizeof(uint64_t));

    return offset >= 0 && size <= (size_t)(INT64_MAX) - (size_t)(offset);
}

static intptr_t pread_chunks(file_t* restrict file, io_cursor_t* restrict cursor, off_t position)
{
    if(!file_seekable(file)) {
        return -ESPIPE;
    }

    if(!range_valid(position, cursor->size)) {
        return -EINVAL;
    }

    return read_chunks(file, cursor, &position);
}

// A write with O_APPEND goes to the end of the file,
// but pwrite writes where it is told to, as POSIX has it
static intptr_t pwrite_chunks(file_t* restrict file, io_cursor_t* restrict cursor, off_t position)
{
    if(!file_seekable(file)) {
        return -ESPIPE;
    }

    if(!range_valid(position, cursor->size)) {
        return -EINVAL;
    }

    return write_chunks(file, cursor, &position, false);
}

static intptr_t read_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, args[1], args[2]);

    return read_chunks(file, &cursor, &file->offset);
}

static intptr_t write_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, args[1], args[2]);

    bool append = file_flags(file) & O_APPEND;

    return write_chunks(file, &cursor, &file->offset, append);
}

static intptr_t pread_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, args[1], args[2]);

    return pread_chunks(file, &cursor, args[3]);
}

static intptr_t pwrite_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, args[1], args[2]);

    return pwrite_chunks(file, &cursor, args[3]);
}

static intptr_t readv_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, args[1], args[2]);

    if(retval < 0) {
        return retval;
    }

    return read_chunks(file, &cursor, &file->offset);
}

static intptr_t writev_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, args[1], args[2]);

    if(retval < 0) {
        return retval;
    }

    bool append = file_flags(file) & O_APPEND;

    return write_chunks(file, &cursor, &file->offset, append);
}

static intptr_t preadv_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, args[1], args[2]);

    if(retval < 0) {
        return retval;
    }

    return pread_chunks(file, &cursor, args[3]);
}

static intptr_t pwritev_file(file_t* restrict file, const uintptr_t* restrict args)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, args[1], args[2]);

    if(retval < 0) {
        return retval;
    }

    return pwrite_chunks(file, &cursor, args[3]);
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

static intptr_t fcntl_setfl(file_t* restrict file, uintptr_t value)
{
    unsigned flags = file_flags(file);

    // As in POSIX, the access mode and the creation flags stay as open left them
    unsigned changed = value & FCNTL_SETTABLE_FLAGS;

    while(!atomic_cmpxchg_weak(&file->flags, &flags, (flags & ~FCNTL_SETTABLE_FLAGS) | changed, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
        // empty
    }

    return 0;
}

static intptr_t fcntl_dupfd(file_t* restrict file, uintptr_t min_fd, uint32_t fd_flags)
{
    thread_t* thread = this_thread();

    if(min_fd >= CONFIG_MAX_FDS) {
        return -EINVAL;
    }

    file_t* copy = file_dupe(file);
    int fd = process_fd_alloc(thread->process, copy, min_fd, fd_flags);

    if(fd < 0) {
        file_close(copy);
    }

    return fd;
}

static intptr_t fcntl_file(file_t* restrict file, const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    unsigned command = args[1];

    switch(command) {
        case F_DUPFD:
            return fcntl_dupfd(file, args[2], 0);
        case F_DUPFD_CLOEXEC:
            return fcntl_dupfd(file, args[2], FD_CLOEXEC);
        case F_GETFD:
            return process_fd_get_flags(thread->process, args[0]);
        case F_SETFD:
            return process_fd_set_flags(thread->process, args[0], args[2] & FD_CLOEXEC);
        case F_GETFL:
            return file_flags(file);
        case F_SETFL:
            return fcntl_setfl(file, args[2]);
        default:
            return -EINVAL;
    }
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

    uint32_t fd_flags = 0;

    if(args[2] & O_CLOEXEC) {
        fd_flags = FD_CLOEXEC;
    }

    file_t* file;
    retval = file_open(base_dir, path, args[2] & ~(uintptr_t)(O_CLOEXEC), args[3] & ~umask, credentials, &file);

    if(base_dir) {
        vnode_decref(base_dir);
    }

    if(retval < 0) {
        return retval;
    }

    int fd = process_fd_alloc(process, file, 0, fd_flags);

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

static intptr_t sys_pread(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &pread_file);
}

define_syscall(sys_pread, SYS_PREAD);

static intptr_t sys_pwrite(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &pwrite_file);
}

define_syscall(sys_pwrite, SYS_PWRITE);

static intptr_t sys_readv(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &readv_file);
}

define_syscall(sys_readv, SYS_READV);

static intptr_t sys_writev(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &writev_file);
}

define_syscall(sys_writev, SYS_WRITEV);

static intptr_t sys_preadv(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &preadv_file);
}

define_syscall(sys_preadv, SYS_PREADV);

static intptr_t sys_pwritev(const uintptr_t* restrict args)
{
    return syscall_with_fd(args, &pwritev_file);
}

define_syscall(sys_pwritev, SYS_PWRITEV);

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

    return process_fd_dup(process, args[0], -1, 0);
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

    return process_fd_dup(process, old_fd, new_fd, 0);
}

define_syscall(sys_dup2, SYS_DUP2);

static intptr_t sys_dup3(const uintptr_t* restrict args)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    int old_fd = args[0];
    int new_fd = args[1];
    uintptr_t flags = args[2];

    if(flags & ~(uintptr_t)(O_CLOEXEC)) {
        return -EINVAL;
    }

    // Unlike dup2, naming the same
    // descriptor twice is an error
    if(old_fd == new_fd) {
        return -EINVAL;
    }

    if(new_fd < 0) {
        return -EBADF;
    }

    uint32_t fd_flags = 0;

    if(flags & O_CLOEXEC) {
        fd_flags = FD_CLOEXEC;
    }

    return process_fd_dup(process, old_fd, new_fd, fd_flags);
}

define_syscall(sys_dup3, SYS_DUP3);

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
