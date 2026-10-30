#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/stat.h>
#include <vex/syscall.h>

#include "ktest.h"

#define PAGE_SIZE   4096
#define SECTOR_SIZE 512
#define FILE_MODE   0644
#define FAT_DEVICE  "/dev/ada0n1p3"
#define SMALL_LIMIT 4
#define STALE_BYTE  0xAA
#define STALE_SIZE  (8 * PAGE_SIZE)
#define VALUE_SIZE  10000
#define LONG_SIZE   20000
#define KEEP_SIZE   40000
#define OFFSET_MAX  0x7FFFFFFFFFFFFFFFL
#define PUNCH_HOLE  0x02
#define NOT_A_MODE  0x100000000L

enum {
    DISK,
    MEMORY,
    FAT,
};

static __k_uint8_t buffer[PAGE_SIZE];

static long make_directory(const char* path)
{
    return ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), 0755);
}

static long remove_path(const char* path, long flags)
{
    return ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(path), flags);
}

static long mount_on(const char* source, const char* target, const char* type, const char* params)
{
    return ktest_syscall6(__k_SYS_MOUNT, (long)(source), (long)(target), (long)(type), 0, (long)(params), 0);
}

static long fallocate(long fd, long mode, long offset, long length)
{
    return ktest_syscall4(__k_SYS_FALLOCATE, fd, mode, offset, length);
}

static struct __k_stat stat_of(long fd)
{
    struct __k_stat info = {};

    ktest_expect(ktest_syscall2(__k_SYS_FSTAT, fd, (long)(&info)), 0);

    return info;
}

static long read_at(long fd, void* data, size_t size, long offset)
{
    return ktest_syscall4(__k_SYS_PREAD, fd, (long)(data), (long)(size), offset);
}

static long write_at(long fd, const void* data, size_t size, long offset)
{
    return ktest_syscall4(__k_SYS_PWRITE, fd, (long)(data), (long)(size), offset);
}

static bool reads_as_zeroes(long fd, long from, long to)
{
    for(long position = from; position < to; position += PAGE_SIZE) {
        long wanted = to - position;

        if(wanted > PAGE_SIZE) {
            wanted = PAGE_SIZE;
        }

        if(read_at(fd, buffer, wanted, position) != wanted) {
            return false;
        }

        for(long i = 0; i < wanted; ++i) {
            if(buffer[i]) {
                return false;
            }
        }
    }

    return true;
}

static void test_growth(const char* path, int kind)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);

    // A range past the end makes the file longer, and the new part reads as zeroes
    ktest_expect(fallocate(fd, 0, 0, VALUE_SIZE), 0);

    struct __k_stat info = stat_of(fd);

    ktest_expect(info.st_size, VALUE_SIZE);
    ktest_expect(reads_as_zeroes(fd, 0, VALUE_SIZE), 1);

    if(kind == DISK) {
        ktest_expect(info.st_blocks >= VALUE_SIZE / SECTOR_SIZE, 1);
    }

    // What the file holds stays, and a range inside the file changes nothing
    ktest_expect(write_at(fd, "hello", 5, 0), 5);
    ktest_expect(fallocate(fd, 0, 0, LONG_SIZE), 0);
    ktest_expect(stat_of(fd).st_size, LONG_SIZE);
    ktest_expect(fallocate(fd, 0, 100, 100), 0);
    ktest_expect(stat_of(fd).st_size, LONG_SIZE);
    ktest_expect(read_at(fd, buffer, 5, 0), 5);
    ktest_expect(ktest_bytes_equal(buffer, "hello", 5), 1);
    ktest_expect(reads_as_zeroes(fd, 5, LONG_SIZE), 1);

    // The range does not have to start at the beginning
    long end = stat_of(fd).st_size;

    ktest_expect(fallocate(fd, 0, end + 50, 50), 0);
    ktest_expect(stat_of(fd).st_size, end + 100);

    ktest_close(fd);
    ktest_expect(remove_path(path, 0), 0);
}

// No filesystem keeps blocks past the end: ext2 has no extents to mark them as not written
static void test_keep_size(const char* path)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);
    ktest_expect(fallocate(fd, 0, 0, LONG_SIZE), 0);
    ktest_expect(fallocate(fd, __k_FALLOC_FL_KEEP_SIZE, 0, KEEP_SIZE), -__k_EOPNOTSUPP);
    ktest_expect(stat_of(fd).st_size, LONG_SIZE);

    ktest_close(fd);
    ktest_expect(remove_path(path, 0), 0);
}

// Blocks that a file gave back may hold its old data, and a range that is made over them has to read as zeroes
static void test_stale_blocks(const char* path)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);

    for(size_t i = 0; i < sizeof(buffer); ++i) {
        buffer[i] = STALE_BYTE;
    }

    for(long position = 0; position < STALE_SIZE; position += PAGE_SIZE) {
        ktest_expect(write_at(fd, buffer, PAGE_SIZE, position), PAGE_SIZE);
    }

    ktest_expect(ktest_syscall1(__k_SYS_FSYNC, fd), 0);
    ktest_expect(ktest_syscall2(__k_SYS_FTRUNCATE, fd, 0), 0);
    ktest_expect(fallocate(fd, 0, 0, STALE_SIZE), 0);
    ktest_expect(reads_as_zeroes(fd, 0, STALE_SIZE), 1);

    ktest_close(fd);
    ktest_expect(remove_path(path, 0), 0);
}

static void test_errors(const char* path)
{
    long fd = ktest_open(path, __k_O_CREAT | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);

    ktest_expect(fallocate(fd, 0, -1, 10), -__k_EINVAL);
    ktest_expect(fallocate(fd, 0, 0, 0), -__k_EINVAL);
    ktest_expect(fallocate(fd, 0, 0, -10), -__k_EINVAL);
    ktest_expect(fallocate(fd, 0, OFFSET_MAX - 1, 5), -__k_EFBIG);
    ktest_expect(fallocate(fd, PUNCH_HOLE | __k_FALLOC_FL_KEEP_SIZE, 0, 10), -__k_EOPNOTSUPP);
    ktest_expect(fallocate(fd, NOT_A_MODE, 0, 10), -__k_EOPNOTSUPP);
    ktest_expect(fallocate(-1, 0, 0, 10), -__k_EBADF);
    ktest_expect(stat_of(fd).st_size, 0);

    ktest_close(fd);

    // A file that is not open for writing
    fd = ktest_open(path, __k_O_RDONLY, 0);
    ktest_expect(fd >= 0, 1);
    ktest_expect(fallocate(fd, 0, 0, 10), -__k_EBADF);
    ktest_close(fd);

    ktest_expect(remove_path(path, 0), 0);

    // A pipe cannot be seeked in
    int pipe_fds[2] = {};

    ktest_expect(ktest_syscall2(__k_SYS_PIPE, (long)(pipe_fds), 0), 0);
    ktest_expect(fallocate(pipe_fds[1], 0, 0, 10), -__k_ESPIPE);
    ktest_close(pipe_fds[0]);
    ktest_close(pipe_fds[1]);
}

static void test_full_tmpfs(const char* mount_point)
{
    char path[64];

    ktest_snprintf(path, sizeof(path), "%s/full", mount_point);

    long fd = ktest_open(path, __k_O_CREAT | __k_O_RDWR, FILE_MODE);
    ktest_expect(fd >= 0, 1);

    // Nothing is taken when the limit is too small, and the size stays
    ktest_expect(fallocate(fd, 0, 0, (SMALL_LIMIT + 1) * PAGE_SIZE), -__k_ENOSPC);
    ktest_expect(stat_of(fd).st_size, 0);
    ktest_expect(fallocate(fd, 0, 0, SMALL_LIMIT * PAGE_SIZE), 0);
    ktest_expect(stat_of(fd).st_size, SMALL_LIMIT * PAGE_SIZE);

    // The pages are paid for, so a write into the range does not fail
    for(long position = 0; position < SMALL_LIMIT * PAGE_SIZE; position += PAGE_SIZE) {
        ktest_expect(write_at(fd, buffer, PAGE_SIZE, position), PAGE_SIZE);
    }

    ktest_expect(fallocate(fd, 0, 0, (SMALL_LIMIT + 1) * PAGE_SIZE), -__k_ENOSPC);

    ktest_close(fd);
    ktest_expect(remove_path(path, 0), 0);
}

int main(void)
{
    ktest_enter_work_directory("fallocate");

    ktest_expect(make_directory("memory"), 0);
    ktest_expect(make_directory("small"), 0);
    ktest_expect(make_directory("fat"), 0);
    ktest_expect(mount_on(nullptr, "memory", "tmpfs", nullptr), 0);
    ktest_expect(mount_on(nullptr, "small", "tmpfs", "size=16k"), 0);
    ktest_expect(mount_on(FAT_DEVICE, "fat", "fat", nullptr), 0);

    test_growth("disk.dat", DISK);
    test_growth("memory/file", MEMORY);
    test_growth("fat/falloc.dat", FAT);

    test_keep_size("disk.dat");
    test_keep_size("memory/file");
    test_keep_size("fat/falloc.dat");

    test_stale_blocks("disk.dat");
    test_stale_blocks("memory/file");
    test_stale_blocks("fat/falloc.dat");

    test_errors("disk.dat");
    test_full_tmpfs("small");

    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)("fat")), 0);
    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)("small")), 0);
    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)("memory")), 0);

    return ktest_result();
}
