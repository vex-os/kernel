#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/limits.h>
#include <vex/stat.h>
#include <vex/syscall.h>

#include "ktest.h"

#define FILE_COUNT            5
#define RECORD_ALIGNMENT      8
#define LARGE_BUFFER_SIZE     1024
#define SMALL_BUFFER_SIZE     48
#define TOO_SMALL_BUFFER_SIZE 8

static long make_directory(const char* path)
{
    return ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), 0755);
}

static long read_entries(long fd, void* buffer, size_t size)
{
    return ktest_syscall3(__k_SYS_GETDENTS, fd, (long)(buffer), (long)(size));
}

// Reads the whole directory with buffers of the given size and returns the count of the entries other than "." and ".."
static long count_entries(const char* path, size_t buffer_size)
{
    alignas(8) char records[LARGE_BUFFER_SIZE];
    long fd = ktest_open(path, __k_O_RDONLY, 0);
    long count = 0;

    ktest_assert(fd >= 0);

    for(;;) {
        long filled = read_entries(fd, records, buffer_size);

        ktest_assert(filled >= 0);

        if(filled == 0) {
            break;
        }

        long position = 0;

        while(position < filled) {
            const struct __k_dirent* entry = (const struct __k_dirent*)(records + position);
            const char* name = (const char*)(entry->d_name);

            ktest_expect(entry->d_reclen % RECORD_ALIGNMENT, 0);
            ktest_assert(entry->d_reclen > 0);

            if(!ktest_string_equal(name, ".") && !ktest_string_equal(name, "..")) {
                count += 1;
            }

            position += entry->d_reclen;
        }

        ktest_expect(position, filled);
    }

    ktest_close(fd);
    return count;
}

static long create_files(const char* directory)
{
    char name[__k_PATH_MAX];

    for(int i = 0; i < FILE_COUNT; ++i) {
        ktest_snprintf(name, sizeof(name), "%s/file-%d", directory, i);

        long fd = ktest_open(name, __k_O_WRONLY | __k_O_CREAT, 0644);

        if(fd < 0) {
            return fd;
        }

        ktest_close(fd);
    }

    return 0;
}

static long node_number(const char* path)
{
    struct __k_stat st = {};

    ktest_assert(ktest_syscall4(__k_SYS_FSTATAT, __k_AT_FDCWD, (long)(path), (long)(&st), 0) == 0);

    return (long)(st.st_ino);
}

// The ".." of a filesystem's root is the root itself, as the filesystem can't see past its mount
static void expect_dots(const char* path, const char* parent)
{
    alignas(8) char records[LARGE_BUFFER_SIZE];
    long fd = ktest_open(path, __k_O_RDONLY, 0);

    ktest_assert(fd >= 0);

    long filled = read_entries(fd, records, sizeof(records));

    ktest_close(fd);
    ktest_assert(filled > 0);

    const struct __k_dirent* dot = (const struct __k_dirent*)(records);

    ktest_assert(filled > dot->d_reclen);

    const struct __k_dirent* dotdot = (const struct __k_dirent*)(records + dot->d_reclen);

    ktest_expect(ktest_string_equal((const char*)(dot->d_name), "."), true);
    ktest_expect(ktest_string_equal((const char*)(dotdot->d_name), ".."), true);
    ktest_expect(dot->d_ino, node_number(path));
    ktest_expect(dotdot->d_ino, node_number(parent));
}

static long read_file_entries(const char* path)
{
    alignas(8) char records[LARGE_BUFFER_SIZE];
    long fd = ktest_open(path, __k_O_RDONLY, 0);

    ktest_assert(fd >= 0);

    long retval = read_entries(fd, records, sizeof(records));

    ktest_close(fd);
    return retval;
}

// The cookie of the next entry must stay valid when the entries before it are unlinked
static long count_while_unlinking(const char* directory)
{
    alignas(8) char records[LARGE_BUFFER_SIZE];
    char path[__k_PATH_MAX];
    long fd = ktest_open(directory, __k_O_RDONLY, 0);
    long count = 0;

    ktest_assert(fd >= 0);

    for(;;) {
        long filled = read_entries(fd, records, SMALL_BUFFER_SIZE);

        ktest_assert(filled >= 0);

        if(filled == 0) {
            break;
        }

        long position = 0;

        while(position < filled) {
            const struct __k_dirent* entry = (const struct __k_dirent*)(records + position);
            const char* name = (const char*)(entry->d_name);

            if(!ktest_string_equal(name, ".") && !ktest_string_equal(name, "..")) {
                ktest_snprintf(path, sizeof(path), "%s/%s", directory, name);
                ktest_expect(ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(path), 0), 0);
                count += 1;
            }

            position += entry->d_reclen;
        }
    }

    ktest_close(fd);
    return count;
}

// On FAT the long names take several slots of the directory
static void test_fat(const char* work_directory)
{
    char mount_point[__k_PATH_MAX];
    char directory[__k_PATH_MAX];
    char file[__k_PATH_MAX];
    char nested[__k_PATH_MAX];
    char above_mount[__k_PATH_MAX];

    ktest_snprintf(mount_point, sizeof(mount_point), "%s/fat", work_directory);
    ktest_snprintf(directory, sizeof(directory), "%s/tmp", mount_point);
    ktest_snprintf(file, sizeof(file), "%s/file-0", directory);
    ktest_snprintf(nested, sizeof(nested), "%s/nested", directory);
    ktest_snprintf(above_mount, sizeof(above_mount), "%s/../..", directory);

    ktest_expect(make_directory(mount_point), 0);
    ktest_expect(ktest_syscall6(__k_SYS_MOUNT, (long)("/dev/ada0n1p3"), (long)(mount_point), (long)("fat"), 0, 0, 0), 0);
    ktest_expect(make_directory(directory), 0);

    // The root has no dot entries on the disk, and the ".." of nested takes its node number from the entry of tmp in the root
    ktest_expect(make_directory(nested), 0);
    expect_dots(mount_point, mount_point);
    expect_dots(directory, mount_point);
    expect_dots(nested, directory);
    ktest_expect(ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(nested), __k_AT_REMOVEDIR), 0);

    // Unlike the ".." record, a path that reaches the root of the mount
    // goes on to the parent of the directory under it
    ktest_expect(node_number(above_mount), node_number(work_directory));

    ktest_expect(create_files(directory), 0);

    // An empty file has cluster 0, which FAT12 and FAT16 take for the root directory
    ktest_expect(read_file_entries(file), -__k_ENOTDIR);

    ktest_expect(count_entries(directory, LARGE_BUFFER_SIZE), FILE_COUNT);
    ktest_expect(count_entries(directory, SMALL_BUFFER_SIZE), FILE_COUNT);
    ktest_expect(count_while_unlinking(directory), FILE_COUNT);
    ktest_expect(count_entries(directory, LARGE_BUFFER_SIZE), 0);

    ktest_expect(ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(directory), __k_AT_REMOVEDIR), 0);
    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)(mount_point)), 0);
}

int main(void)
{
    const char* work_directory = ktest_enter_work_directory("getdents");

    ktest_expect(make_directory("dir"), 0);
    ktest_expect(create_files("dir"), 0);

    expect_dots("dir", ".");
    expect_dots("/dev", "/dev");

    ktest_expect(count_entries("dir", LARGE_BUFFER_SIZE), FILE_COUNT);

    // A buffer that holds a record or two makes the call return in several parts
    ktest_expect(count_entries("dir", SMALL_BUFFER_SIZE), FILE_COUNT);

    ktest_expect(make_directory("unlinked"), 0);
    ktest_expect(create_files("unlinked"), 0);
    ktest_expect(count_while_unlinking("unlinked"), FILE_COUNT);
    ktest_expect(count_entries("unlinked", LARGE_BUFFER_SIZE), 0);

    // The first record doesn't fit
    alignas(8) char records[LARGE_BUFFER_SIZE];
    long fd = ktest_open("dir", __k_O_RDONLY, 0);

    ktest_assert(fd >= 0);
    ktest_expect(read_entries(fd, records, TOO_SMALL_BUFFER_SIZE), -__k_EINVAL);

    // The failed call doesn't move the position
    ktest_assert(read_entries(fd, records, sizeof(records)) > 0);
    ktest_close(fd);

    test_fat(work_directory);

    return ktest_result();
}
