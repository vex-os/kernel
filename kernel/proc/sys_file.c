#include <vex/address.h>
#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/file_lock.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/path.h>
#include <vex/pipe.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/stat.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/uio.h>
#include <vex/vnode.h>

#include "syscall_internal.h"

#define FCNTL_SETTABLE_FLAGS (O_APPEND | O_NONBLOCK)

static_assert(FILE_IO_CHUNK <= PAGE_SIZE);

typedef struct io_cursor_s io_cursor_t;

// Where the data of a read or write goes to or comes from: one buffer, or the buffers of an iovec array
// in user memory. The array is read one element at a time, as it can be larger than anything on the stack
struct io_cursor_s {
    user_addr_t iov; // Zero elements for a single buffer
    size_t count;
    size_t index;
    user_addr_t base; // What is left of the current buffer
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

static void io_cursor_init_buffer(io_cursor_t* restrict cursor, user_addr_t base, size_t size)
{
    cursor->iov = user_addr(0);
    cursor->count = 0;
    cursor->index = 0;
    cursor->base = base;
    cursor->left = size;
    cursor->budget = 0;
    cursor->size = size;
}

// The sum of the lengths has to fit in the result of the call, which is checked before any data moves
static int io_cursor_init_vector(io_cursor_t* restrict cursor, user_addr_t iov, size_t count)
{
    static_assert(sizeof(size_t) == sizeof(uint64_t));

    if(count > IOV_MAX) {
        return -EINVAL;
    }

    size_t size = 0;

    for(size_t i = 0; i < count; ++i) {
        iovec_t element;
        user_addr_t source = user_addr_add(iov, i * sizeof(element));
        ssize_t copied = user_copyin(&element, source, sizeof(element));

        if(copied < 0) {
            return copied;
        }

        if(element.len > (size_t)(INT64_MAX) - (size)) { // clang-format antics
            return -EINVAL;
        }

        size += element.len;
    }

    cursor->iov = iov;
    cursor->count = count;
    cursor->index = 0;
    cursor->base = user_addr(0);
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
        user_addr_t source = user_addr_add(cursor->iov, cursor->index * sizeof(iov));
        ssize_t copied = user_copyin(&iov, source, sizeof(iov));

        if(copied < 0) {
            return copied;
        }

        size_t length = iov.len;

        if(length > cursor->budget) {
            length = cursor->budget;
        }

        cursor->index += 1;
        cursor->budget -= length;
        cursor->base = user_addr(iov.base);
        cursor->left = length;
    }

    return 1;
}

static void io_cursor_advance(io_cursor_t* restrict cursor, size_t size)
{
    cursor->base = user_addr_add(cursor->base, size);
    cursor->left -= size;
}

// A device such as a tty or a pipe hands out what it has at once,
// and asking it again would wait for more to come in
static bool hands_out_at_once(const file_t* restrict file)
{
    unsigned type = file->node->type;

    return type == VNODE_CHAR_DEV || type == VNODE_FIFO;
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

static intptr_t sys_read(file_t* restrict file, user_addr_t buffer, size_t count)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, buffer, count);

    return read_chunks(file, &cursor, &file->offset);
}

define_syscall_fd(sys_read, SYS_READ, 3);

static intptr_t sys_write(file_t* restrict file, user_addr_t buffer, size_t count)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, buffer, count);

    bool append = file_flags(file) & O_APPEND;

    return write_chunks(file, &cursor, &file->offset, append);
}

define_syscall_fd(sys_write, SYS_WRITE, 3);

static intptr_t sys_pread(file_t* restrict file, user_addr_t buffer, size_t count, off_t offset)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, buffer, count);

    return pread_chunks(file, &cursor, offset);
}

define_syscall_fd(sys_pread, SYS_PREAD, 4);

static intptr_t sys_pwrite(file_t* restrict file, user_addr_t buffer, size_t count, off_t offset)
{
    io_cursor_t cursor;
    io_cursor_init_buffer(&cursor, buffer, count);

    return pwrite_chunks(file, &cursor, offset);
}

define_syscall_fd(sys_pwrite, SYS_PWRITE, 4);

static intptr_t sys_readv(file_t* restrict file, user_addr_t iov, size_t count)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, iov, count);

    if(retval < 0) {
        return retval;
    }

    return read_chunks(file, &cursor, &file->offset);
}

define_syscall_fd(sys_readv, SYS_READV, 3);

static intptr_t sys_writev(file_t* restrict file, user_addr_t iov, size_t count)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, iov, count);

    if(retval < 0) {
        return retval;
    }

    bool append = file_flags(file) & O_APPEND;

    return write_chunks(file, &cursor, &file->offset, append);
}

define_syscall_fd(sys_writev, SYS_WRITEV, 3);

static intptr_t sys_preadv(file_t* restrict file, user_addr_t iov, size_t count, off_t offset)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, iov, count);

    if(retval < 0) {
        return retval;
    }

    return pread_chunks(file, &cursor, offset);
}

define_syscall_fd(sys_preadv, SYS_PREADV, 4);

static intptr_t sys_pwritev(file_t* restrict file, user_addr_t iov, size_t count, off_t offset)
{
    io_cursor_t cursor;
    int retval = io_cursor_init_vector(&cursor, iov, count);

    if(retval < 0) {
        return retval;
    }

    return pwrite_chunks(file, &cursor, offset);
}

define_syscall_fd(sys_pwritev, SYS_PWRITEV, 4);

static intptr_t sys_seek(file_t* restrict file, off_t offset, int origin)
{
    return file_seek(file, offset, origin);
}

define_syscall_fd(sys_seek, SYS_SEEK, 3);

static intptr_t sys_ftruncate(file_t* restrict file, off_t length)
{
    thread_t* thread = this_thread();

    return file_truncate(file, thread->credentials, length);
}

define_syscall_fd(sys_ftruncate, SYS_FTRUNCATE, 2);

static intptr_t sys_fstat(file_t* restrict file, user_addr_t stat)
{
    stat_t st;
    int retval = vnode_stat(file->node, &st);

    if(retval < 0) {
        return retval;
    }

    ssize_t copied = user_copyout(stat, &st, sizeof(st));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

define_syscall_fd(sys_fstat, SYS_FSTAT, 2);

static intptr_t sys_ioctl(file_t* restrict file, uint32_t request, uintptr_t argument)
{
    return file_ioctl(file, request, argument);
}

define_syscall_fd(sys_ioctl, SYS_IOCTL, 3);

static intptr_t sys_fsync(file_t* restrict file)
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

define_syscall_fd(sys_fsync, SYS_FSYNC, 1);

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

static int file_end(file_t* restrict file, off_t* restrict out)
{
    stat_t st;
    int retval = vnode_stat(file->node, &st);

    if(retval < 0) {
        return retval;
    }

    *out = st.st_size;
    return 0;
}

static int lock_base(file_t* restrict file, int whence, off_t* restrict out)
{
    switch(whence) {
        case SEEK_SET:
            *out = 0;
            return 0;

        case SEEK_CUR:
            *out = atomic_load(&file->offset, ATOMIC_RELAXED);
            return 0;

        case SEEK_END:
            return file_end(file, out);

        default:
            return -EINVAL;
    }
}

// A positive length counts forward from the start, zero runs on with no end, and a negative one counts back
static int lock_range(file_t* restrict file, const flock_t* restrict lock, file_lock_request_t* restrict out)
{
    off_t base;
    int retval = lock_base(file, lock->l_whence, &base);

    if(retval < 0) {
        return retval;
    }

    off_t start;

    if(__builtin_add_overflow(base, lock->l_start, &start)) {
        return -EOVERFLOW;
    }

    off_t end = FILE_LOCK_END;

    if(lock->l_len > 0 && __builtin_add_overflow(start, lock->l_len - 1, &end)) {
        return -EOVERFLOW;
    }

    if(lock->l_len < 0) {
        end = start - 1;
        start += lock->l_len;
    }

    if(start < 0) {
        return -EINVAL;
    }

    out->start = start;
    out->end = end;
    return 0;
}

static int lock_request_of(file_t* restrict file, const flock_t* restrict lock, file_lock_request_t* restrict out)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    bool known = lock->l_type == F_RDLCK || lock->l_type == F_WRLCK || lock->l_type == F_UNLCK;

    if(!known) {
        return -EINVAL;
    }

    out->owner = process;
    out->pid = process->pid;
    out->type = lock->l_type;

    return lock_range(file, lock, out);
}

static intptr_t fcntl_getlk(file_t* restrict file, user_addr_t address)
{
    flock_t lock;
    ssize_t copied = user_copyin(&lock, address, sizeof(lock));

    if(copied < 0) {
        return copied;
    }

    file_lock_request_t request = {};
    int retval = lock_request_of(file, &lock, &request);

    if(retval < 0) {
        return retval;
    }

    if(request.type == F_UNLCK) {
        return -EINVAL;
    }

    lock.l_type = F_UNLCK;

    if(file_lock_test(file->node, &request)) {
        lock.l_type = request.type;
        lock.l_whence = SEEK_SET;
        lock.l_start = request.start;
        lock.l_len = 0;
        lock.l_pid = request.pid;

        if(request.end != FILE_LOCK_END) {
            lock.l_len = request.end - request.start + 1;
        }
    }

    copied = user_copyout(address, &lock, sizeof(lock));

    if(copied < 0) {
        return copied;
    }

    return 0;
}

// A read lock takes a file open for reading, and a write lock one open for writing
static intptr_t fcntl_setlk(file_t* restrict file, user_addr_t address, bool wait)
{
    flock_t lock;
    ssize_t copied = user_copyin(&lock, address, sizeof(lock));

    if(copied < 0) {
        return copied;
    }

    file_lock_request_t request = {};
    int retval = lock_request_of(file, &lock, &request);

    if(retval < 0) {
        return retval;
    }

    unsigned flags = file_flags(file);
    bool readable = file_flags_allow_read(flags);
    bool writable = file_flags_allow_write(flags);

    if(request.type == F_RDLCK && !readable) {
        return -EBADF;
    }

    if(request.type == F_WRLCK && !writable) {
        return -EBADF;
    }

    return file_lock_set(file->node, &request, wait);
}

static intptr_t fcntl_command(file_t* restrict file, int fd, unsigned command, uintptr_t argument)
{
    thread_t* thread = this_thread();

    switch(command) {
        case F_DUPFD:
            return fcntl_dupfd(file, argument, 0);
        case F_DUPFD_CLOEXEC:
            return fcntl_dupfd(file, argument, FD_CLOEXEC);
        case F_GETFD:
            return process_fd_get_flags(thread->process, fd);
        case F_SETFD:
            return process_fd_set_flags(thread->process, fd, argument & FD_CLOEXEC);
        case F_GETFL:
            return file_flags(file);
        case F_SETFL:
            return fcntl_setfl(file, argument);
        case F_GETLK:
            return fcntl_getlk(file, user_addr(argument));
        case F_SETLK:
            return fcntl_setlk(file, user_addr(argument), false);
        case F_SETLKW:
            return fcntl_setlk(file, user_addr(argument), true);
        default:
            return -EINVAL;
    }
}

static intptr_t sys_fcntl(int fd, unsigned command, uintptr_t argument)
{
    file_t* file;
    intptr_t retval = syscall_fd_get(fd, &file);

    if(retval < 0) {
        return retval;
    }

    retval = fcntl_command(file, fd, command, argument);
    file_close(file);

    return retval;
}

define_syscall(sys_fcntl, SYS_FCNTL, 3);

static intptr_t sys_openat(int dirfd, const char* restrict path, uintptr_t flags, mode_t mode)
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

    uint32_t fd_flags = 0;

    if(flags & O_CLOEXEC) {
        fd_flags = FD_CLOEXEC;
    }

    file_t* file;
    retval = file_open(base_dir, path, flags & ~(uintptr_t)(O_CLOEXEC), mode & ~umask, credentials, &file);

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

define_syscall_dirfd(sys_openat, SYS_OPENAT, 4);

static intptr_t sys_close(int fd)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    return process_fd_release(process, fd);
}

define_syscall(sys_close, SYS_CLOSE, 1);

static intptr_t sys_dup(int old_fd, int new_fd, uintptr_t flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(uintptr_t)(O_CLOEXEC)) {
        return -EINVAL;
    }

    // The same descriptor twice is an error. dup2 allows it, and
    // the C library checks that the descriptor is open
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

define_syscall(sys_dup, SYS_DUP, 3);

static intptr_t sys_pipe(user_addr_t fds, uintptr_t flags)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    if(flags & ~(uintptr_t)(O_CLOEXEC | O_NONBLOCK)) {
        return -EINVAL;
    }

    uint32_t fd_flags = 0;

    if(flags & O_CLOEXEC) {
        fd_flags = FD_CLOEXEC;
    }

    file_t* read_end;
    file_t* write_end;
    int retval = pipe_create(flags & O_NONBLOCK, thread->credentials, &read_end, &write_end);

    if(retval < 0) {
        return retval;
    }

    int read_fd = process_fd_alloc(process, read_end, 0, fd_flags);

    if(read_fd < 0) {
        file_close(read_end);
        file_close(write_end);
        return read_fd;
    }

    int write_fd = process_fd_alloc(process, write_end, 0, fd_flags);

    if(write_fd < 0) {
        process_fd_release(process, read_fd);
        file_close(write_end);
        return write_fd;
    }

    int32_t pair[2];
    pair[0] = read_fd;
    pair[1] = write_fd;

    ssize_t copied = user_copyout(fds, pair, sizeof(pair));

    // Another thread may have closed or reused the descriptors by now. This is its own doing
    if(copied < 0) {
        process_fd_release(process, read_fd);
        process_fd_release(process, write_fd);
        return copied;
    }

    return 0;
}

define_syscall(sys_pipe, SYS_PIPE, 2);
