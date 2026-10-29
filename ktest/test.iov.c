#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/limits.h>
#include <vex/syscall.h>
#include <vex/uio.h>

#include "ktest.h"

#define PAGE_SIZE 4096
#define BIG_SIZE  (2 * PAGE_SIZE + 300)

static char big[BIG_SIZE];
static char copy[BIG_SIZE];

static struct __k_iovec vec(const void* base, size_t length)
{
    struct __k_iovec iov;

    iov.base = (__k_uintptr_t)(base);
    iov.len = length;

    return iov;
}

static long read_vector(long fd, const struct __k_iovec* iov, long count)
{
    return ktest_syscall3(__k_SYS_READV, fd, (long)(iov), count);
}

static long write_vector(long fd, const struct __k_iovec* iov, long count)
{
    return ktest_syscall3(__k_SYS_WRITEV, fd, (long)(iov), count);
}

static long pread_vector(long fd, const struct __k_iovec* iov, long count, long offset)
{
    return ktest_syscall4(__k_SYS_PREADV, fd, (long)(iov), count, offset);
}

static long pwrite_vector(long fd, const struct __k_iovec* iov, long count, long offset)
{
    return ktest_syscall4(__k_SYS_PWRITEV, fd, (long)(iov), count, offset);
}

static long seek_to(long fd, long offset, long origin)
{
    return ktest_syscall3(__k_SYS_SEEK, fd, offset, origin);
}

static void test_small(void)
{
    long fd = ktest_open("iov.txt", __k_O_RDWR | __k_O_CREAT, 0644);
    char first[4] = {};
    char second[3] = {};
    char third[8] = {};

    ktest_assert(fd >= 0);

    // An empty buffer in the middle does nothing
    struct __k_iovec out[] = { vec("head", 4), vec("", 0), vec("-body", 5) };

    ktest_expect(write_vector(fd, out, 3), 9);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), 9);
    ktest_expect(seek_to(fd, 0, __k_SEEK_SET), 0);

    struct __k_iovec in[] = { vec(first, sizeof(first)), vec(second, sizeof(second)), vec(third, sizeof(third)) };

    // The read stops at the end of the file, in the middle of the last buffer
    ktest_expect(read_vector(fd, in, 3), 9);
    ktest_expect(ktest_bytes_equal(first, "head", 4), 1);
    ktest_expect(ktest_bytes_equal(second, "-bo", 3), 1);
    ktest_expect(ktest_bytes_equal(third, "dy", 2), 1);
    ktest_expect(read_vector(fd, in, 3), 0);

    // The offset of the file stays where it was
    struct __k_iovec at[] = { vec(first, 2), vec(second, 2) };

    ktest_expect(pread_vector(fd, at, 2, 2), 4);
    ktest_expect(ktest_bytes_equal(first, "ad", 2), 1);
    ktest_expect(ktest_bytes_equal(second, "-b", 2), 1);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), 9);

    struct __k_iovec patch[] = { vec("X", 1), vec("YZ", 2) };

    ktest_expect(pwrite_vector(fd, patch, 2, 1), 3);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), 9);
    ktest_expect(seek_to(fd, 0, __k_SEEK_SET), 0);
    ktest_expect(ktest_read(fd, third, 8), 8);
    ktest_expect(ktest_bytes_equal(third, "hXYZ-bod", 8), 1);

    ktest_expect(write_vector(fd, out, 0), 0);
    ktest_expect(read_vector(fd, in, 0), 0);

    // POSIX: with O_APPEND, writev goes to the end and pwritev to the given offset
    long appender = ktest_open("iov.txt", __k_O_WRONLY | __k_O_APPEND, 0);

    ktest_assert(appender >= 0);
    ktest_expect(write_vector(appender, patch, 2), 3);
    ktest_expect(seek_to(fd, 0, __k_SEEK_END), 12);
    ktest_expect(pwrite_vector(appender, patch, 2, 0), 3);
    ktest_expect(seek_to(fd, 0, __k_SEEK_END), 12);
    ktest_close(appender);

    ktest_close(fd);
}

static void test_big(void)
{
    long fd = ktest_open("iov.bin", __k_O_RDWR | __k_O_CREAT, 0644);

    ktest_assert(fd >= 0);

    for(size_t i = 0; i < BIG_SIZE; ++i) {
        big[i] = (char)(i % 251);
    }

    // The buffers add up to more than a page, and one of them crosses a page border
    struct __k_iovec out[] = { vec(big, 100), vec(&big[100], PAGE_SIZE), vec(&big[100 + PAGE_SIZE], BIG_SIZE - 100 - PAGE_SIZE) };

    ktest_expect(write_vector(fd, out, 3), BIG_SIZE);
    ktest_expect(seek_to(fd, 0, __k_SEEK_SET), 0);

    struct __k_iovec in[] = { vec(copy, PAGE_SIZE + 1), vec(&copy[PAGE_SIZE + 1], BIG_SIZE - PAGE_SIZE - 1) };

    ktest_expect(read_vector(fd, in, 2), BIG_SIZE);
    ktest_expect(ktest_bytes_equal(big, copy, BIG_SIZE), 1);

    ktest_close(fd);
}

static void test_errors(void)
{
    char buffer[4] = {};
    long fd = ktest_open("iov.err", __k_O_RDWR | __k_O_CREAT, 0644);

    ktest_assert(fd >= 0);

    struct __k_iovec good = vec("abcd", 4);
    struct __k_iovec huge[] = { vec(buffer, (size_t)(0x7FFFFFFFFFFFFFFFULL)), vec(buffer, 1) };

    ktest_expect(write_vector(fd, &good, __k_IOV_MAX + 1), -__k_EINVAL);
    ktest_expect(write_vector(fd, huge, 2), -__k_EINVAL);
    ktest_expect(write_vector(fd, nullptr, 1), -__k_EFAULT);
    ktest_expect(read_vector(fd, nullptr, 1), -__k_EFAULT);
    ktest_expect(write_vector(1000, &good, 1), -__k_EBADF);

    // What came before the fault is written, and the call reports it
    struct __k_iovec faulty[] = { vec("abcd", 4), vec(nullptr, 4) };

    ktest_expect(write_vector(fd, faulty, 2), 4);
    ktest_expect(write_vector(fd, &faulty[1], 1), -__k_EFAULT);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), 4);

    ktest_expect(pread_vector(fd, &good, 1, -1), -__k_EINVAL);
    ktest_expect(pwrite_vector(fd, &good, 1, -1), -__k_EINVAL);

    ktest_close(fd);

    // The access mode of the descriptor still counts
    long reader = ktest_open("iov.err", __k_O_RDONLY, 0);

    ktest_assert(reader >= 0);
    ktest_expect(write_vector(reader, &good, 1), -__k_EBADF);
    ktest_close(reader);

    // A stream has no position to read or write at
    long log = ktest_open("/dev/kplog", __k_O_RDONLY, 0);

    ktest_assert(log >= 0);
    ktest_expect(pread_vector(log, &good, 1, 0), -__k_ESPIPE);
    ktest_expect(pwrite_vector(log, &good, 1, 0), -__k_ESPIPE);
    ktest_close(log);
}

int main(void)
{
    ktest_enter_work_directory("iov");

    test_small();
    test_big();
    test_errors();

    return ktest_result();
}
