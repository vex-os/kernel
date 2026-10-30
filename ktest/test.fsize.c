#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/resource.h>
#include <vex/signal.h>
#include <vex/stat.h>
#include <vex/syscall.h>

#include "ktest.h"

#define BIT(sig)   (1UL << ((sig) - 1))
#define FILE_MODE  0644
#define FILE_LIMIT 10000
#define CHUNK_SIZE 6000
#define PIPE_BYTES 100

static char buffer[CHUNK_SIZE];

static long remove_path(const char* path)
{
    return ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(path), 0);
}

static long write_at(long fd, size_t size, long offset)
{
    return ktest_syscall4(__k_SYS_PWRITE, fd, (long)(buffer), (long)(size), offset);
}

static long truncate_to(long fd, long size)
{
    return ktest_syscall2(__k_SYS_FTRUNCATE, fd, size);
}

static long fallocate(long fd, long offset, long length)
{
    return ktest_syscall4(__k_SYS_FALLOCATE, fd, 0, offset, length);
}

static long size_of(long fd)
{
    struct __k_stat info = {};

    ktest_expect(ktest_syscall2(__k_SYS_FSTAT, fd, (long)(&info)), 0);

    return info.st_size;
}

static void limit_size(unsigned long current, unsigned long maximum)
{
    struct __k_rlimit limit = {};
    limit.rlim_cur = current;
    limit.rlim_max = maximum;

    ktest_expect(ktest_syscall2(__k_SYS_SETRLIMIT, __k_RLIMIT_FSIZE, (long)(&limit)), 0);
}

// SIGXFSZ is blocked, so that it stays pending and a zero timeout finds it
static long take_signal(void)
{
    __k_sigset_t set = BIT(__k_SIGXFSZ);
    struct __k_timespec timeout = {};

    return ktest_syscall3(__k_SYS_SIGTIMEDWAIT, (long)(&set), 0, (long)(&timeout));
}

static void test_write(const char* path)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_TRUNC | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);

    // A write that crosses the limit stops at it, with no signal
    ktest_expect(ktest_write(fd, buffer, CHUNK_SIZE), CHUNK_SIZE);
    ktest_expect(ktest_write(fd, buffer, CHUNK_SIZE), FILE_LIMIT - CHUNK_SIZE);
    ktest_expect(size_of(fd), FILE_LIMIT);
    ktest_expect(take_signal(), -__k_EAGAIN);

    // The next one fails and signals the thread
    ktest_expect(ktest_write(fd, buffer, 1), -__k_EFBIG);
    ktest_expect(take_signal(), __k_SIGXFSZ);
    ktest_expect(write_at(fd, 1, FILE_LIMIT + 100), -__k_EFBIG);
    ktest_expect(take_signal(), __k_SIGXFSZ);

    // An empty write changes nothing, so it does not fail
    ktest_expect(ktest_write(fd, buffer, 0), 0);
    ktest_expect(take_signal(), -__k_EAGAIN);

    // A write inside the limit still works
    ktest_expect(write_at(fd, 100, 0), 100);
    ktest_expect(size_of(fd), FILE_LIMIT);

    ktest_close(fd);

    // O_APPEND moves to the end first, and the limit counts from there
    fd = ktest_open(path, __k_O_RDWR | __k_O_APPEND, 0);
    ktest_expect(fd >= 0, 1);
    ktest_expect(truncate_to(fd, FILE_LIMIT - 10), 0);
    ktest_expect(ktest_write(fd, buffer, 100), 10);
    ktest_expect(size_of(fd), FILE_LIMIT);
    ktest_expect(ktest_write(fd, buffer, 1), -__k_EFBIG);
    ktest_expect(take_signal(), __k_SIGXFSZ);

    ktest_close(fd);
    ktest_expect(remove_path(path), 0);
}

static void test_size_change(const char* path)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_TRUNC | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);

    ktest_expect(truncate_to(fd, FILE_LIMIT + 1), -__k_EFBIG);
    ktest_expect(take_signal(), __k_SIGXFSZ);
    ktest_expect(size_of(fd), 0);
    ktest_expect(truncate_to(fd, FILE_LIMIT), 0);
    ktest_expect(truncate_to(fd, 100), 0);

    ktest_expect(fallocate(fd, FILE_LIMIT - 10, 11), -__k_EFBIG);
    ktest_expect(take_signal(), __k_SIGXFSZ);
    ktest_expect(size_of(fd), 100);
    ktest_expect(fallocate(fd, 0, FILE_LIMIT), 0);
    ktest_expect(size_of(fd), FILE_LIMIT);

    ktest_close(fd);
    ktest_expect(remove_path(path), 0);
}

// A file that is longer than the limit already may shrink, or change inside its size, as in Linux
static void test_long_file(const char* path, unsigned long maximum)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_TRUNC | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);

    limit_size(maximum, maximum);
    ktest_expect(truncate_to(fd, 3 * FILE_LIMIT), 0);
    limit_size(FILE_LIMIT, maximum);

    ktest_expect(truncate_to(fd, 2 * FILE_LIMIT), 0);
    ktest_expect(fallocate(fd, FILE_LIMIT, FILE_LIMIT), 0);
    ktest_expect(size_of(fd), 2 * FILE_LIMIT);
    ktest_expect(take_signal(), -__k_EAGAIN);

    ktest_expect(truncate_to(fd, 2 * FILE_LIMIT + 1), -__k_EFBIG);
    ktest_expect(take_signal(), __k_SIGXFSZ);

    // A write is checked against the limit and not against the size
    ktest_expect(write_at(fd, 100, 0), 100);
    ktest_expect(write_at(fd, 1, FILE_LIMIT), -__k_EFBIG);
    ktest_expect(take_signal(), __k_SIGXFSZ);

    ktest_close(fd);
    ktest_expect(remove_path(path), 0);
}

// Only a regular file has a size limit
static void test_pipe(unsigned long maximum)
{
    int fds[2] = {};

    limit_size(1, maximum);
    ktest_expect(ktest_syscall2(__k_SYS_PIPE, (long)(fds), 0), 0);
    ktest_expect(ktest_write(fds[1], buffer, PIPE_BYTES), PIPE_BYTES);
    ktest_expect(take_signal(), -__k_EAGAIN);
    ktest_close(fds[0]);
    ktest_close(fds[1]);
}

int main(void)
{
    ktest_enter_work_directory("fsize");

    struct __k_rlimit original = {};
    ktest_expect(ktest_syscall2(__k_SYS_GETRLIMIT, __k_RLIMIT_FSIZE, (long)(&original)), 0);
    ktest_expect(original.rlim_cur, __k_RLIM_INFINITY);

    __k_sigset_t mask = BIT(__k_SIGXFSZ);
    ktest_syscall3(__k_SYS_SIGPROCMASK, __k_SIG_BLOCK, (long)(&mask), 0);

    limit_size(FILE_LIMIT, original.rlim_max);

    test_write("write.dat");
    test_size_change("size.dat");
    test_long_file("long.dat", original.rlim_max);
    test_pipe(original.rlim_max);

    return ktest_result();
}
