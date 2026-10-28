#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/syscall.h>

#include "ktest.h"

#define PAGE_SIZE   4096
#define BIG_SIZE    (3 * PAGE_SIZE + 100)
#define SPAN_SIZE   9000
#define SPAN_START  100
#define WRITE_START 500

static char big[BIG_SIZE];
static char span[SPAN_SIZE];

static long read_at(long fd, void* buffer, size_t size, long offset)
{
    return ktest_syscall4(__k_SYS_PREAD, fd, (long)(buffer), (long)(size), offset);
}

static long write_at(long fd, const void* buffer, size_t size, long offset)
{
    return ktest_syscall4(__k_SYS_PWRITE, fd, (long)(buffer), (long)(size), offset);
}

static long seek_to(long fd, long offset, long origin)
{
    return ktest_syscall3(__k_SYS_SEEK, fd, offset, origin);
}

static void test_small(void)
{
    long fd = ktest_open("small.txt", __k_O_RDWR | __k_O_CREAT, 0644);
    char buffer[8] = {};

    ktest_assert(fd >= 0);
    ktest_expect(ktest_write(fd, "0123456789ABCDEF", 16), 16);
    ktest_expect(seek_to(fd, 0, __k_SEEK_SET), 0);

    // The offset of the file stays where it was
    ktest_expect(read_at(fd, buffer, 4, 4), 4);
    ktest_expect(ktest_bytes_equal(buffer, "4567", 4), 1);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), 0);
    ktest_expect(ktest_read(fd, buffer, 4), 4);
    ktest_expect(ktest_bytes_equal(buffer, "0123", 4), 1);

    ktest_expect(write_at(fd, "xy", 2, 2), 2);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), 4);
    ktest_expect(read_at(fd, buffer, 6, 0), 6);
    ktest_expect(ktest_bytes_equal(buffer, "01xy45", 6), 1);

    // Nothing to read at the end of the file or after it
    ktest_expect(read_at(fd, buffer, 4, 16), 0);
    ktest_expect(read_at(fd, buffer, 4, 1000), 0);
    ktest_expect(read_at(fd, buffer, 0, 0), 0);

    // A write after the end leaves a hole of zeros
    ktest_expect(write_at(fd, "Z", 1, 20), 1);
    ktest_expect(seek_to(fd, 0, __k_SEEK_END), 21);
    ktest_expect(read_at(fd, buffer, 5, 16), 5);
    ktest_expect(ktest_bytes_equal(buffer, "\0\0\0\0Z", 5), 1);

    ktest_expect(read_at(fd, buffer, 4, -1), -__k_EINVAL);
    ktest_expect(write_at(fd, "a", 1, -1), -__k_EINVAL);
    ktest_expect(read_at(fd, nullptr, 4, 0), -__k_EFAULT);

    ktest_close(fd);

    // The access mode of the descriptor still counts
    long reader = ktest_open("small.txt", __k_O_RDONLY, 0);
    long writer = ktest_open("small.txt", __k_O_WRONLY, 0);

    ktest_assert(reader >= 0 && writer >= 0);
    ktest_expect(write_at(reader, "a", 1, 0), -__k_EBADF);
    ktest_expect(read_at(writer, buffer, 1, 0), -__k_EBADF);
    ktest_expect(read_at(1000, buffer, 1, 0), -__k_EBADF);

    // POSIX: with O_APPEND the data still goes to the offset that was given
    long appender = ktest_open("small.txt", __k_O_WRONLY | __k_O_APPEND, 0);

    ktest_assert(appender >= 0);
    ktest_expect(write_at(appender, "AA", 2, 0), 2);
    ktest_expect(read_at(reader, buffer, 4, 0), 4);
    ktest_expect(ktest_bytes_equal(buffer, "AAxy", 4), 1);
    ktest_expect(seek_to(reader, 0, __k_SEEK_END), 21);

    ktest_close(appender);
    ktest_close(writer);
    ktest_close(reader);
}

static void test_big(void)
{
    long fd = ktest_open("big.bin", __k_O_RDWR | __k_O_CREAT, 0644);

    ktest_assert(fd >= 0);

    for(size_t i = 0; i < BIG_SIZE; ++i) {
        big[i] = (char)(i % 251);
    }

    ktest_expect(ktest_write(fd, big, BIG_SIZE), BIG_SIZE);

    // More than one chunk goes through in a single call
    ktest_expect(read_at(fd, span, SPAN_SIZE, SPAN_START), SPAN_SIZE);
    ktest_expect(ktest_bytes_equal(span, &big[SPAN_START], SPAN_SIZE), 1);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), BIG_SIZE);

    // The read stops at the end of the file
    ktest_expect(read_at(fd, span, SPAN_SIZE, BIG_SIZE - 10), 10);

    for(size_t i = 0; i < SPAN_SIZE; ++i) {
        span[i] = (char)(~(i % 241));
    }

    ktest_expect(write_at(fd, span, SPAN_SIZE, WRITE_START), SPAN_SIZE);
    ktest_expect(seek_to(fd, 0, __k_SEEK_CUR), BIG_SIZE);

    memset(big, 0, sizeof(big));
    ktest_expect(read_at(fd, big, BIG_SIZE, 0), BIG_SIZE);
    ktest_expect(ktest_bytes_equal(&big[WRITE_START], span, SPAN_SIZE), 1);
    ktest_expect(big[WRITE_START - 1], (char)((WRITE_START - 1) % 251));
    ktest_expect(big[WRITE_START + SPAN_SIZE], (char)((WRITE_START + SPAN_SIZE) % 251));

    ktest_close(fd);
}

static void test_streams(void)
{
    char buffer[4];

    // A device that is a stream has no position to read or write at
    long log = ktest_open("/dev/kplog", __k_O_RDONLY, 0);

    ktest_assert(log >= 0);
    ktest_expect(read_at(log, buffer, sizeof(buffer), 0), -__k_ESPIPE);
    ktest_expect(write_at(log, buffer, sizeof(buffer), 0), -__k_ESPIPE);
    ktest_expect(seek_to(log, 0, __k_SEEK_SET), -__k_ESPIPE);
    ktest_close(log);

    long tree = ktest_open("/dev/hwtree", __k_O_RDONLY, 0);

    ktest_assert(tree >= 0);
    ktest_expect(read_at(tree, buffer, sizeof(buffer), 0), -__k_ESPIPE);
    ktest_expect(seek_to(tree, 0, __k_SEEK_CUR), -__k_ESPIPE);
    ktest_close(tree);
}

int main(void)
{
    ktest_enter_work_directory("pread");

    test_small();
    test_big();
    test_streams();

    return ktest_result();
}
